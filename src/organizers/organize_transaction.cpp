/**
 * Copyright (c) 2011-2019 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/organizers/organize_transaction.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include <bitcoin/system.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/transaction_pool.hpp>
#include <bitcoin/blockchain/settings.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::database;
using namespace bc::system;
using namespace std::placeholders;

#define NAME "organize_transaction"

organize_transaction::organize_transaction(prioritized_mutex& mutex,
    dispatcher& priority_dispatch, threadpool&, fast_chain& chain,
    transaction_pool& pool, const settings& settings)
  : fast_chain_(chain),
    mutex_(mutex),
    stopped_(true),
    settings_(settings),
    pool_(pool),
    validator_(priority_dispatch, fast_chain_, settings)
{
}

// Properties.
//-----------------------------------------------------------------------------

bool organize_transaction::stopped() const
{
    return stopped_;
}

// Start/stop sequences.
//-----------------------------------------------------------------------------

bool organize_transaction::start()
{
    stopped_ = false;
    validator_.start();
    return true;
}

bool organize_transaction::stop()
{
    validator_.stop();
    stopped_ = true;
    return true;
}

// Organize sequence.
//-----------------------------------------------------------------------------
// This runs in single thread normal priority except for validation fan-outs.
// Therefore fan-outs may use all threads in the priority threadpool.

// This is called from block_chain::organize.
void organize_transaction::organize(transaction_const_ptr tx,
    result_handler handler, uint64_t max_money)
{
    code error_code;

    // Checks that are independent of chain state.
    if ((error_code = validator_.check(tx, max_money)))
    {
        handler(error_code);
        return;
    }

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    mutex_.lock_low_priority();

    if (stopped())
    {
        mutex_.unlock_low_priority();
        //---------------------------------------------------------------------
        handler(error::service_stopped);
        return;
    }

    // The pool is safe for filtering only, so protect by critical section.
    // This locates only unconfirmed transactions discovered since startup.
    const auto exists = pool_.exists(tx);

    // See symmetry with header memory pool.
    // The tx is already memory pooled (nothing to do).
    if (exists)
    {
        mutex_.unlock_low_priority();
        //---------------------------------------------------------------------
        handler(error::duplicate_transaction);
        return;
    }

    // Reset the reusable promise.
    resume_ = {};

    const result_handler complete =
        std::bind(&organize_transaction::signal_completion,
            this, _1);

    const auto accept_handler =
        std::bind(&organize_transaction::handle_accept,
            this, _1, tx, complete);

    // Checks that are dependent on chain state and prevouts.
    validator_.accept(tx, accept_handler);

    // Wait on completion signal.
    // This is necessary in order to continue on a non-priority thread.
    // If we do not wait on the original thread there may be none left.
    error_code = resume_.get_future().get();

    mutex_.unlock_low_priority();
    ///////////////////////////////////////////////////////////////////////////

    // Invoke caller handler outside of critical section.
    handler(error_code);
}

// private
void organize_transaction::signal_completion(const code& ec)
{
    // This must be protected so that it is properly cleared.
    // Signal completion, which results in original handler invoke with code.
    resume_.set_value(ec);
}

// Verify sub-sequence.
//-----------------------------------------------------------------------------

// private
void organize_transaction::handle_accept(const code& ec,
    transaction_const_ptr tx, result_handler handler)
{
    // The tx may exist in the store in any state except confirmed or verified.
    // Either state implies that the tx exists and is valid for its context.

    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        handler(ec);
        return;
    }

    // Policy.
    if (!sufficient_fee(tx))
    {
        handler(error::insufficient_fee);
        return;
    }

    // Policy.
    if (tx->is_dusty(settings_.minimum_output_satoshis))
    {
        handler(error::dusty_transaction);
        return;
    }

    const auto connect_handler =
        std::bind(&organize_transaction::handle_connect,
            this, _1, tx, handler);

    // Checks that include script metadata.
    validator_.connect(tx, connect_handler);
}

// private
void organize_transaction::handle_connect(const code& ec,
    transaction_const_ptr tx, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        handler(ec);
        return;
    }

    // TODO: add to pool_.
    //#########################################################################
    const auto error_code = fast_chain_.store(tx);
    //#########################################################################

    if (error_code)
    {
        LOG_FATAL(LOG_BLOCKCHAIN)
            << "Failure writing transaction to store, is now corrupted: "
            << error_code.message();

        handler(error_code);
        return;
    }

    handler(error_code);
}

// Utility.
//-----------------------------------------------------------------------------

bool organize_transaction::sufficient_fee(transaction_const_ptr tx) const
{
    static const auto version = message::version::level::canonical;
    const auto byte_fee = settings_.byte_fee_satoshis;
    const auto sigop_fee = settings_.sigop_fee_satoshis;

    // Guard against summing signed values by testing independently.
    if (byte_fee == 0.0f && sigop_fee == 0.0f)
        return true;

    // TODO: incorporate tx weight discount.
    // TODO: incorporate bip16 and bip141 in sigops parmaterization.
    // TODO: this is a second pass on size and sigops, implement cache.
    // This at least prevents uncached calls when zero fee is configured.
    auto byte = byte_fee > 0 ? byte_fee * tx->serialized_size(version) : 0;
    auto sigop = sigop_fee > 0 ? sigop_fee * tx->signature_operations() : 0;

    // Require at least one satoshi per tx if there are any fees configured.
    auto price = std::max(uint64_t(1), static_cast<uint64_t>(byte + sigop));
    const auto paid = tx->fees();

    // Skip logging if fee is sufficent.
    if (paid >= price)
        return true;

    // TODO: optimize out second serialized_size signature_operations calls.
    LOG_DEBUG(LOG_BLOCKCHAIN)
        << "Transaction [" << encode_hash(tx->hash()) << "] "
        << "bytes: " << tx->serialized_size(version) << " "
        << "sigops: " << tx->signature_operations() << " "
        << "price: " << price << " "
        << "paid: " << tx->fees();

    return false;
}

} // namespace blockchain
} // namespace libbitcoin
