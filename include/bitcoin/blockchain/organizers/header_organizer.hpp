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
#ifndef LIBBITCOIN_BLOCKCHAIN_HEADER_ORGANIZER_HPP
#define LIBBITCOIN_BLOCKCHAIN_HEADER_ORGANIZER_HPP

#include <atomic>
#include <memory>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/header_branch.hpp>
#include <bitcoin/blockchain/pools/header_pool.hpp>
#include <bitcoin/blockchain/validate/validate_header.hpp>

namespace libbitcoin {
namespace blockchain {

/// This class is thread safe.
/// Organises headers to the store via the header pool.
class BCB_API header_organizer
{
public:
    typedef handle0 result_handler;
    typedef std::shared_ptr<header_organizer> ptr;

    /// Construct an instance.
    header_organizer(prioritized_mutex& mutex, dispatcher& priority_dispatch,
        threadpool& threads, fast_chain& chain, header_pool& pool,
        const bool scrypt, const bc::settings& bitcoin_settings);

    // Start/stop the organizer.
    bool start();
    bool stop();

    /// validate and organize a header into header pool and store.
    void organize(header_const_ptr header, result_handler handler);

protected:
    bool stopped() const;

private:
    // Verify sub-sequence.
    void handle_accept(const code& ec, header_branch::ptr branch, result_handler handler);
    void handle_complete(const code& ec, result_handler handler);

    // These are thread safe.
    fast_chain& fast_chain_;
    prioritized_mutex& mutex_;
    std::atomic<bool> stopped_;
    header_pool& pool_;
    validate_header validator_;
};

} // namespace blockchain
} // namespace libbitcoin

#endif
