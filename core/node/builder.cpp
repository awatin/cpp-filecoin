/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "builder.hpp"

#include <boost/di/extension/scopes/shared.hpp>

#include <libp2p/injector/gossip_injector.hpp>
#include <libp2p/protocol/identify/identify.hpp>
#include <libp2p/protocol/identify/identify_delta.hpp>
#include <libp2p/protocol/identify/identify_push.hpp>

#include "api/make.hpp"
#include "blockchain/block_validator/impl/block_validator_impl.hpp"
#include "blockchain/impl/weight_calculator_impl.hpp"
#include "clock/impl/chain_epoch_clock_impl.hpp"
#include "clock/impl/utc_clock_impl.hpp"
#include "crypto/bls/impl/bls_provider_impl.hpp"
#include "crypto/secp256k1/impl/secp256k1_provider_impl.hpp"
#include "power/impl/power_table_impl.hpp"
#include "storage/chain/impl/chain_store_impl.hpp"
#include "storage/chain/msg_waiter.hpp"
#include "storage/indexdb/indexdb.hpp"
#include "storage/ipfs/graphsync/impl/graphsync_impl.hpp"
#include "storage/ipfs/impl/in_memory_datastore.hpp"
#include "storage/keystore/impl/in_memory/in_memory_keystore.hpp"
#include "storage/mpool/mpool.hpp"
#include "vm/interpreter/impl/interpreter_impl.hpp"

namespace fc::node {

  outcome::result<NodeObjects> createNodeObjects(const Config &config) {
    NodeObjects o;

    auto injector = libp2p::injector::makeGossipInjector<
        boost::di::extension::shared_config>(
        boost::di::bind<clock::UTCClock>.template to<clock::UTCClockImpl>(),
        libp2p::injector::useGossipConfig(config.gossip_config));

    o.io_context = injector.create<std::shared_ptr<boost::asio::io_context>>();
    o.scheduler =
        injector.create<std::shared_ptr<libp2p::protocol::Scheduler>>();

    o.host = injector.create<std::shared_ptr<libp2p::Host>>();

    auto identify_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::Identify>>();
    auto identify_push_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::IdentifyPush>>();
    auto identify_delta_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::IdentifyDelta>>();

    o.utc_clock = injector.create<std::shared_ptr<clock::UTCClock>>();

    // TODO: genesis time
    o.chain_epoch_clock = std::make_shared<clock::ChainEpochClockImpl>(
        clock::Time{std::chrono::nanoseconds(0)}.unixTime());

    // TODO - switch on real storage after debugging all the stuff
    o.ipfs_datastore = std::make_shared<storage::ipfs::InMemoryDatastore>();



    //    auto block_service =
    //        std::make_shared<storage::ipfs::IpfsBlockService>(o.ipfs_datastore);

    auto weight_calculator =
        std::make_shared<blockchain::weight::WeightCalculatorImpl>(
            o.ipfs_datastore);

    auto power_table = std::make_shared<power::PowerTableImpl>();

    auto bls_provider = std::make_shared<crypto::bls::BlsProviderImpl>();

    auto secp_provider =
        std::make_shared<crypto::secp256k1::Secp256k1ProviderImpl>();

    // TODO: persistent keystore
    auto key_store = std::make_shared<storage::keystore::InMemoryKeyStore>(
        bls_provider, secp_provider);

    auto vm_interpreter = std::make_shared<vm::interpreter::InterpreterImpl>();

    o.block_validator =
        std::make_shared<blockchain::block_validator::BlockValidatorImpl>(
            o.ipfs_datastore,
            o.utc_clock,
            o.chain_epoch_clock,
            weight_calculator,
            power_table,
            bls_provider,
            secp_provider,
            vm_interpreter);

    /*

    auto chain_store_res = storage::blockchain::ChainStoreImpl::create(
        block_service, o.block_validator, weight_calculator);
    if (!chain_store_res) {
      return chain_store_res.error();
    }
    o.chain_store = std::move(chain_store_res.value());
*/
    // TODO feed genesis into store

    o.gossip =
        injector.create<std::shared_ptr<libp2p::protocol::gossip::Gossip>>();

    o.graphsync = std::make_shared<storage::ipfs::graphsync::GraphsyncImpl>(
        o.host, o.scheduler);

    // ARTEM, why all the todos above are without jira ids?

    // TODO Artem - replace two stubs below with proper implementations
    auto mpool = storage::mpool::Mpool::create(o.ipfs_datastore, o.chain_store);
    auto msg_waiter =
        storage::blockchain::MsgWaiter::create(o.ipfs_datastore, o.chain_store);

    // TODO Artem - pass the correct interpreter below
    o.api = std::make_shared<api::Api>(api::makeImpl(o.chain_store,
                                                     weight_calculator,
                                                     o.ipfs_datastore,
                                                     bls_provider,
                                                     mpool,
                                                     vm_interpreter,
                                                     msg_waiter,
                                                     key_store));

    return o;
  }

}  // namespace fc::node
