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
#ifndef LIBBITCOIN_BLOCKCHAIN_ORGANIZE_TRANSACTION_HPP
#define LIBBITCOIN_BLOCKCHAIN_ORGANIZE_TRANSACTION_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <bitcoin/system.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/interface/safe_chain.hpp>
#include <bitcoin/blockchain/pools/transaction_pool.hpp>
#include <bitcoin/blockchain/settings.hpp>
#include <bitcoin/blockchain/validate/validate_transaction.hpp>

namespace libbitcoin {
namespace blockchain {

/// This class is thread safe.
/// Organises transactions via the tx metadata pool to the store.
class BCB_API organize_transaction
{
public:
    typedef system::handle0 result_handler;
    typedef std::shared_ptr<organize_transaction> ptr;
    typedef safe_chain::inventory_fetch_handler inventory_fetch_handler;
    typedef safe_chain::merkle_block_fetch_handler merkle_block_fetch_handler;

    /// Construct an instance.
    organize_transaction(system::prioritized_mutex& mutex,
        system::dispatcher& priority_dispatch, system::threadpool& threads,
        fast_chain& chain, transaction_pool& pool, const settings& settings);

    // Start/stop the organizer.
    bool start();
    bool stop();

    /// validate and organize a transaction into tx metadata pool and store.
    void organize(system::transaction_const_ptr tx, result_handler handler,
        uint64_t max_money);

protected:
    bool stopped() const;
    bool sufficient_fee(system::transaction_const_ptr tx) const;

private:
    // Verify sub-sequence.
    void handle_accept(const system::code& ec,
        system::transaction_const_ptr tx, result_handler handler);
    void handle_connect(const system::code& ec,
        system::transaction_const_ptr tx, result_handler handler);
    void signal_completion(const system::code& ec);

    // These are thread safe.
    fast_chain& fast_chain_;
    system::prioritized_mutex& mutex_;
    std::atomic<bool> stopped_;
    std::promise<system::code> resume_;
    const settings& settings_;
    transaction_pool& pool_;
    validate_transaction validator_;
};

} // namespace blockchain
} // namespace libbitcoin

#endif
