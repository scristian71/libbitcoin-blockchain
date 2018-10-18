/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
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
#include <bitcoin/blockchain/organizers/header_organizer.hpp>

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/header_branch.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;
using namespace bc::config;
using namespace bc::database;
using namespace std::placeholders;

#define NAME "header_organizer"

header_organizer::header_organizer(prioritized_mutex& mutex,
    dispatcher& priority_dispatch, threadpool&, fast_chain& chain,
    header_pool& pool, bool scrypt, const bc::settings& bitcoin_settings)
  : fast_chain_(chain),
    mutex_(mutex),
    stopped_(true),
    pool_(pool),
    validator_(priority_dispatch, chain, scrypt, bitcoin_settings)
{
}

// Properties.
//-----------------------------------------------------------------------------

bool header_organizer::stopped() const
{
    return stopped_;
}

// Start/stop sequences.
//-----------------------------------------------------------------------------

bool header_organizer::start()
{
    stopped_ = false;
    validator_.start();
    return true;
}

bool header_organizer::stop()
{
    validator_.stop();
    stopped_ = true;
    return true;
}

// Organize sequence.
//-----------------------------------------------------------------------------
// This runs in single thread normal priority.

// This is called from block_chain::organize.
void header_organizer::organize(header_const_ptr header,
    result_handler handler)
{
    code error_code;

    // Checks that are independent of chain state.
    if ((error_code = validator_.check(header)))
    {
        handler(error_code);
        return;
    }

    const result_handler complete =
        std::bind(&header_organizer::handle_complete,
            this, _1, handler);

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    mutex_.lock_high_priority();

    // The pool is safe for filtering only, so protect by critical section.
    // This sets height and presumes the fork point is an indexed header.
    const auto branch = pool_.get_branch(header);

    // See symmetry with tx metadata memory pool.
    // The header is already memory pooled (nothing to do).
    if (branch->empty())
    {
        complete(error::duplicate_block);
        return;
    }

    const auto accept_handler =
        std::bind(&header_organizer::handle_accept,
            this, _1, branch, complete);

    // Checks that are dependent on chain state.
    validator_.accept(branch, accept_handler);
}

// private
void header_organizer::handle_complete(const code& ec, result_handler handler)
{
    mutex_.unlock_high_priority();
    ///////////////////////////////////////////////////////////////////////////

    // Invoke caller handler outside of critical section.
    handler(ec);
}

// Verify sub-sequence.
//-----------------------------------------------------------------------------

// private
void header_organizer::handle_accept(const code& ec, header_branch::ptr branch,
    result_handler handler)
{
    // The header may exist in the store in any not-invalid state.
    // An invalid state causes an error result and block rejection.

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

    // The top block is valid even if the branch has insufficient work.
    const auto top = branch->top();
    const auto work = branch->work();
    uint256_t required_work;

    // This stops before the height or at the work level, which ever is first.
    if (!fast_chain_.get_work(required_work, work, branch->height(), true))
    {
        handler(error::operation_failed);
        return;
    }

    // Consensus.
    if (work <= required_work)
    {
        pool_.add(top, branch->top_height());
        handler(error::insufficient_work);
        return;
    }

    //#########################################################################
    const auto error_code = fast_chain_.reorganize(branch->fork_point(),
        branch->headers());
    //#########################################################################

    if (error_code)
    {
        LOG_FATAL(LOG_BLOCKCHAIN)
            << "Failure writing header to store, is now corrupted: "
            << error_code.message();

        handler(error_code);
        return;
    }

    handler(error_code);
}

} // namespace blockchain
} // namespace libbitcoin
