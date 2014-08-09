/**
 * Copyright (c) 2011-2014 sx developers (see AUTHORS)
 *
 * This file is part of sx.
 *
 * sx is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <sx/command/send-tx-p2p.hpp>

#include <iostream>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <bitcoin/bitcoin.hpp>
#include <sx/async_client.hpp>
#include <sx/define.hpp>
#include <sx/utility/utility.hpp>
#include <sx/serializer/transaction.hpp>

using namespace bc;
using namespace sx;
using namespace sx::extension;
using namespace sx::serializer;

const static char* endline = "\n";

//static void output_to_file(std::ofstream& file, log_level level,
//    const std::string& domain, const std::string& body)
//{
//    if (!body.empty())
//        file << boost::format(SX_SEND_TX_P2P_OUTPUT) % level_repr(level) % body
//            << std::endl;
//}
//
//static void output_cerr_and_file(std::ofstream& file, log_level level,
//    const std::string& domain, const std::string& body)
//{
//    if (!body.empty())
//        std::cerr << boost::format(SX_SEND_TX_P2P_OUTPUT) % level_repr(level) %
//            body << std::endl;
//}

//// TODO: look into behavior if logging is invoked but log is not initialized.
//static void bind_logging(const boost::filesystem::path& debug,
//    const boost::filesystem::path& error)
//{
//    if (!debug.empty())
//    {
//        std::ofstream debug_file(debug.generic_string());
//        log_debug().set_output_function(
//            std::bind(output_to_file, std::ref(debug_file),
//            ph::_1, ph::_2, ph::_3));
//
//        log_info().set_output_function(
//            std::bind(output_to_file, std::ref(debug_file),
//            ph::_1, ph::_2, ph::_3));
//    }
//
//    if (!error.empty())
//    {
//        std::ofstream error_file(error.generic_string());
//        log_warning().set_output_function(
//            std::bind(output_to_file, std::ref(error_file),
//            ph::_1, ph::_2, ph::_3));
//
//        log_error().set_output_function(
//            std::bind(output_cerr_and_file, std::ref(error_file),
//            ph::_1, ph::_2, ph::_3));
//
//        log_fatal().set_output_function(
//            std::bind(output_cerr_and_file, std::ref(error_file),
//            ph::_1, ph::_2, ph::_3));
//    }
//}

static void handle_signal(int signal)
{
    // Can't pass args using lambda capture for a simple function pointer.
    // This means there's no way to terminate without using a global variable
    // or process termination. Since the variable would screw with testing all 
    // other methods we opt for process termination here.
    exit(console_result::failure);
}

// Started protocol, node discovery complete.
static void handle_start(callback_args& args)
{
    args.output() << SX_SEND_TX_P2P_START_OKAY;
}

// Fetched a number of connections.
static void handle_check(callback_args& args, size_t connection_count,
    size_t node_count)
{
    args.output() << boost::format(SX_SEND_TX_P2P_CHECK_OKAY) %
        connection_count << endline;
    if (connection_count >= node_count)
        args.stopped() = true;
}

// Send completed.
static void handle_sent(callback_args& args)
{
    args.output() << boost::format(SX_SEND_TX_P2P_SEND_OKAY) % now()
        << std::endl;
}

// Send tx to another Bitcoin node.
static void handle_send(callback_args& args, const std::error_code& error, 
    channel_ptr node, protocol& prot, transaction_type& tx)
{
    const auto sent_handler = [&args](const std::error_code& error)
    {
        handle_error(args, error, SX_SEND_TX_P2P_SEND_FAIL);
        handle_sent(args);
    };

    const auto send_handler = [&args](const std::error_code& error,
        channel_ptr node, protocol& prot, transaction_type& tx)
    {
        handle_error(args, error, SX_SEND_TX_P2P_SETUP_FAIL);
        handle_send(args, error, node, prot, tx);
    };

    args.output() << boost::format(SX_SEND_TX_P2P_SETUP_OKAY) % transaction(tx)
        << std::endl;
    node->send(tx, sent_handler);
    prot.subscribe_channel(std::bind(send_handler, ph::_1, ph::_2,
        std::ref(prot), std::ref(tx)));
}

console_result send_tx_p2p::invoke(std::ostream& output, std::ostream& cerr)
{
    // Bound parameters.
    //const auto& debug_log = get_logging_debug_setting();
    //const auto& error_log = get_logging_error_setting();
    const auto& node_count = get_nodes_option();
    const auto& transactions = get_transactions_argument();
    HANDLE_MULTIPLE_NOT_IMPLEMENTED(transactions);
    const transaction_type& tx = transactions.front();

    //bind_logging(debug_log, error_log);
    callback_args args(cerr, output);

    const auto start_handler = [&args](const std::error_code& error)
    {
        handle_error(args, error, SX_SEND_TX_P2P_START_FAIL);
        handle_start(args);
    };

    const auto check_handler = [&args](const std::error_code& error,
        size_t connection_count, size_t node_count)
    {
        handle_error(args, error, SX_SEND_TX_P2P_CHECK_FAIL);
        handle_check(args, connection_count, node_count);
    };

    const auto send_handler = [&args](const std::error_code& error,
        channel_ptr node, protocol& prot, transaction_type& tx)
    {
        handle_error(args, error, SX_SEND_TX_P2P_SETUP_FAIL);
        handle_send(args, error, node, prot, tx);
    };

    const auto stop_handler = [](const std::error_code&)
    {
    };

    async_client client(*this, 4);

    // Create dependencies for our protocol object.
    auto& pool = client.get_threadpool();
    hosts hst(pool);
    handshake hs(pool);
    network net(pool);

    // protocol service.
    protocol prot(pool, hst, hs, net);
    prot.set_max_outbound(node_count * 6);

    // Perform node discovery if needed and then creating connections.
    prot.start(start_handler);
    prot.subscribe_channel(
        std::bind(send_handler, ph::_1, ph::_2, std::ref(prot), tx));

    // Catch C signals for stopping the program.
    signal(SIGABRT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    // Check the connection count every 2 seconds.
    const auto work = [&prot, &node_count, &check_handler]
    { 
        prot.fetch_connection_count(
            std::bind(check_handler, ph::_1, ph::_2, node_count));
    };

    client.poll(args.stopped(), 2000, work);
    prot.stop(stop_handler);
    
    return args.result();
}