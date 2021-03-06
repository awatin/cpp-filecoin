/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "api/make.hpp"

#include <boost/algorithm/string.hpp>
#include <libp2p/peer/peer_id.hpp>

#include "blockchain/production/block_producer.hpp"
#include "proofs/proofs.hpp"
#include "storage/hamt/hamt.hpp"
#include "vm/actor/builtin/account/account_actor.hpp"
#include "vm/actor/builtin/init/init_actor.hpp"
#include "vm/actor/builtin/market/actor.hpp"
#include "vm/actor/builtin/miner/types.hpp"
#include "vm/actor/builtin/storage_power/storage_power_actor_state.hpp"
#include "vm/actor/impl/invoker_impl.hpp"
#include "vm/message/impl/message_signer_impl.hpp"
#include "vm/runtime/env.hpp"
#include "vm/state/impl/state_tree_impl.hpp"

namespace fc::api {
  using primitives::kChainEpochUndefined;
  using vm::actor::kInitAddress;
  using vm::actor::kStorageMarketAddress;
  using vm::actor::kStoragePowerAddress;
  using vm::actor::builtin::account::AccountActorState;
  using vm::actor::builtin::init::InitActorState;
  using vm::actor::builtin::market::DealState;
  using vm::actor::builtin::miner::MinerActorState;
  using vm::actor::builtin::storage_power::StoragePowerActorState;
  using InterpreterResult = vm::interpreter::Result;
  using crypto::randomness::DomainSeparationTag;
  using crypto::signature::BlsSignature;
  using libp2p::peer::PeerId;
  using primitives::block::MsgMeta;
  using vm::isVMExitCode;
  using vm::normalizeVMExitCode;
  using vm::VMExitCode;
  using vm::actor::InvokerImpl;
  using vm::runtime::Env;
  using vm::state::StateTreeImpl;
  using connection_t = boost::signals2::connection;
  using MarketActorState = vm::actor::builtin::market::State;

  constexpr EpochDuration kWinningPoStSectorSetLookback{10};

  struct TipsetContext {
    Tipset tipset;
    StateTreeImpl state_tree;
    boost::optional<InterpreterResult> interpreted;

    auto marketState() {
      return state_tree.state<MarketActorState>(kStorageMarketAddress);
    }

    auto minerState(const Address &address) {
      return state_tree.state<MinerActorState>(address);
    }

    auto powerState() {
      return state_tree.state<StoragePowerActorState>(kStoragePowerAddress);
    }

    auto initState() {
      return state_tree.state<InitActorState>(kInitAddress);
    }

    outcome::result<Address> accountKey(const Address &id) {
      // TODO(turuslan): error if not account
      OUTCOME_TRY(state, state_tree.state<AccountActorState>(id));
      return state.address;
    }
  };

  Api makeImpl(std::shared_ptr<ChainStore> chain_store,
               std::shared_ptr<WeightCalculator> weight_calculator,
               std::shared_ptr<Ipld> ipld,
               std::shared_ptr<Mpool> mpool,
               std::shared_ptr<Interpreter> interpreter,
               std::shared_ptr<MsgWaiter> msg_waiter,
               std::shared_ptr<Beaconizer> beaconizer,
               std::shared_ptr<KeyStore> key_store) {
    auto tipsetContext = [=](const TipsetKey &tipset_key,
                             bool interpret =
                                 false) -> outcome::result<TipsetContext> {
      Tipset tipset;
      if (tipset_key.cids.empty()) {
        tipset = chain_store->heaviestTipset();
      } else {
        OUTCOME_TRYA(tipset, Tipset::load(*ipld, tipset_key.cids));
      }
      TipsetContext context{tipset, {ipld, tipset.getParentStateRoot()}, {}};
      if (interpret) {
        OUTCOME_TRY(result, interpreter->interpret(ipld, tipset));
        context.state_tree = {ipld, result.state_root};
        context.interpreted = result;
      }
      return context;
    };
    auto getLookbackTipSetForRound =
        [=](auto tipset, auto epoch) -> outcome::result<TipsetContext> {
      auto lookback{
          std::max(ChainEpoch{0}, epoch - kWinningPoStSectorSetLookback)};
      while (tipset.height > static_cast<uint64_t>(lookback)) {
        OUTCOME_TRYA(tipset, tipset.loadParent(*ipld));
      }
      OUTCOME_TRY(result, interpreter->interpret(ipld, tipset));
      return TipsetContext{
          std::move(tipset), {ipld, std::move(result.state_root)}, {}};
    };
    auto getSectorsForWinningPoSt =
        [=](auto &miner,
            auto &state,
            auto &post_rand) -> outcome::result<std::vector<SectorInfo>> {
      std::vector<SectorInfo> sectors;
      OUTCOME_TRY(seal_type,
                  primitives::sector::sealProofTypeFromSectorSize(
                      state.info.sector_size));
      OUTCOME_TRY(win_type,
                  primitives::sector::getRegisteredWinningPoStProof(seal_type));
      OUTCOME_TRY(state.visitProvingSet([&](auto id, auto &info) {
        sectors.push_back({win_type, id, info.info.sealed_cid});
      }));
      if (!sectors.empty()) {
        OUTCOME_TRY(indices,
                    proofs::Proofs::generateWinningPoStSectorChallenge(
                        win_type, miner.getId(), post_rand, sectors.size()));
        std::vector<SectorInfo> result;
        for (auto &i : indices) {
          result.push_back(sectors[i]);
        }
      }
      return sectors;
    };
    return {
        .AuthNew = {[](auto) {
          return Buffer{1, 2, 3};
        }},
        .ChainGetBlock = {[=](auto &block_cid) {
          return ipld->getCbor<BlockHeader>(block_cid);
        }},
        .ChainGetBlockMessages = {[=](auto &block_cid)
                                      -> outcome::result<BlockMessages> {
          BlockMessages messages;
          OUTCOME_TRY(block, ipld->getCbor<BlockHeader>(block_cid));
          OUTCOME_TRY(meta, ipld->getCbor<MsgMeta>(block.messages));
          OUTCOME_TRY(meta.bls_messages.visit(
              [&](auto, auto &cid) -> outcome::result<void> {
                OUTCOME_TRY(message, ipld->getCbor<UnsignedMessage>(cid));
                messages.bls.push_back(std::move(message));
                messages.cids.push_back(cid);
                return outcome::success();
              }));
          OUTCOME_TRY(meta.secp_messages.visit(
              [&](auto, auto &cid) -> outcome::result<void> {
                OUTCOME_TRY(message, ipld->getCbor<SignedMessage>(cid));
                messages.secp.push_back(std::move(message));
                messages.cids.push_back(cid);
                return outcome::success();
              }));
          return messages;
        }},
        .ChainGetGenesis = {[=]() -> outcome::result<Tipset> {
          return Tipset::create({chain_store->getGenesis()});
        }},
        .ChainGetNode = {[=](auto &path) -> outcome::result<IpldObject> {
          std::vector<std::string> parts;
          boost::split(parts, path, [](auto c) { return c == '/'; });
          if (parts.size() < 3 || !parts[0].empty() || parts[1] != "ipfs") {
            return TodoError::kError;
          }
          OUTCOME_TRY(root, CID::fromString(parts[2]));
          return getNode(ipld, root, gsl::make_span(parts).subspan(3));
        }},
        .ChainGetMessage = {[=](auto &cid) -> outcome::result<UnsignedMessage> {
          auto res = ipld->getCbor<SignedMessage>(cid);
          if (!res.has_error()) {
            return res.value().message;
          }

          return ipld->getCbor<UnsignedMessage>(cid);
        }},
        .ChainGetParentMessages =
            {[=](auto &block_cid) -> outcome::result<std::vector<CidMessage>> {
              std::vector<CidMessage> messages;
              OUTCOME_TRY(block, ipld->getCbor<BlockHeader>(block_cid));
              for (auto &parent_cid : block.parents) {
                OUTCOME_TRY(parent, ipld->getCbor<BlockHeader>(parent_cid));
                OUTCOME_TRY(meta, ipld->getCbor<MsgMeta>(parent.messages));
                OUTCOME_TRY(meta.bls_messages.visit(
                    [&](auto, auto &cid) -> outcome::result<void> {
                      OUTCOME_TRY(message, ipld->getCbor<UnsignedMessage>(cid));
                      messages.push_back({cid, std::move(message)});
                      return outcome::success();
                    }));
                OUTCOME_TRY(meta.secp_messages.visit(
                    [&](auto, auto &cid) -> outcome::result<void> {
                      OUTCOME_TRY(message, ipld->getCbor<SignedMessage>(cid));
                      messages.push_back({cid, std::move(message.message)});
                      return outcome::success();
                    }));
              }
              return messages;
            }},
        .ChainGetParentReceipts =
            {[=](auto &block_cid)
                 -> outcome::result<std::vector<MessageReceipt>> {
              OUTCOME_TRY(block, ipld->getCbor<BlockHeader>(block_cid));
              return adt::Array<MessageReceipt>{block.parent_message_receipts,
                                                ipld}
                  .values();
            }},
        .ChainGetRandomness =
            {[=](auto &tipset_key, auto tag, auto epoch, auto &entropy)
                 -> outcome::result<Randomness> {
              OUTCOME_TRY(context, tipsetContext(tipset_key));
              return context.tipset.randomness(*ipld, tag, epoch, entropy);
            }},
        .ChainGetTipSet = {[=](auto &tipset_key) {
          return Tipset::load(*ipld, tipset_key.cids);
        }},
        .ChainGetTipSetByHeight = {[=](auto height2, auto &tipset_key)
                                       -> outcome::result<Tipset> {
          // TODO(turuslan): use height index from chain store
          // TODO(turuslan): return genesis if height is zero
          auto height = static_cast<uint64_t>(height2);
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          auto &tipset{context.tipset};
          if (tipset.height < height) {
            return TodoError::kError;
          }
          while (tipset.height > height) {
            OUTCOME_TRY(parent, tipset.loadParent(*ipld));
            if (parent.height < height) {
              break;
            }
            tipset = std::move(parent);
          }
          return std::move(tipset);
        }},
        .ChainHead = {[=]() { return chain_store->heaviestTipset(); }},
        .ChainNotify = {[=]() {
          auto channel = std::make_shared<Channel<std::vector<HeadChange>>>();
          auto cnn = std::make_shared<connection_t>();
          *cnn = chain_store->subscribeHeadChanges([=](auto &change) {
            if (!channel->write({change})) {
              assert(cnn->connected());
              cnn->disconnect();
            }
          });
          return Chan{std::move(channel)};
        }},
        .ChainReadObj = {[=](const auto &cid) { return ipld->get(cid); }},
        // TODO(turuslan): FIL-165 implement method
        .ChainSetHead = {},
        .ChainTipSetWeight = {[=](auto &tipset_key)
                                  -> outcome::result<TipsetWeight> {
          OUTCOME_TRY(tipset, Tipset::load(*ipld, tipset_key.cids));
          return weight_calculator->calculateWeight(tipset);
        }},
        // TODO(turuslan): FIL-165 implement method
        .ClientFindData = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientHasLocal = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientImport = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientListImports = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientQueryAsk = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientRetrieve = {},
        // TODO(turuslan): FIL-165 implement method
        .ClientStartDeal = {},
        // TODO(turuslan): FIL-165 implement method
        .MarketEnsureAvailable = {},
        .MinerCreateBlock = {[=](auto &t) -> outcome::result<BlockWithCids> {
          OUTCOME_TRY(context, tipsetContext(t.parents, true));
          OUTCOME_TRY(miner_state, context.minerState(t.miner));
          OUTCOME_TRY(block,
                      blockchain::production::generate(
                          *interpreter, ipld, std::move(t)));

          OUTCOME_TRY(block_signable, codec::cbor::encode(block.header));
          OUTCOME_TRY(worker_key, context.accountKey(miner_state.info.worker));
          OUTCOME_TRY(block_sig, key_store->sign(worker_key, block_signable));
          block.header.block_sig = block_sig;

          BlockWithCids block2;
          block2.header = block.header;
          for (auto &msg : block.bls_messages) {
            OUTCOME_TRY(cid, ipld->setCbor(msg));
            block2.bls_messages.emplace_back(std::move(cid));
          }
          for (auto &msg : block.secp_messages) {
            OUTCOME_TRY(cid, ipld->setCbor(msg));
            block2.secp_messages.emplace_back(std::move(cid));
          }
          return block2;
        }},
        .MinerGetBaseInfo =
            {[=](auto &miner, auto epoch, auto &tipset_key)
                 -> outcome::result<boost::optional<MiningBaseInfo>> {
              OUTCOME_TRY(context, tipsetContext(tipset_key, true));
              MiningBaseInfo info;
              OUTCOME_TRYA(info.prev_beacon,
                           context.tipset.latestBeacon(*ipld));
              OUTCOME_TRYA(
                  info.beacons,
                  beaconizer->beaconEntriesForBlock(epoch, info.prev_beacon));
              OUTCOME_TRY(lookback,
                          getLookbackTipSetForRound(context.tipset, epoch));
              OUTCOME_TRY(state, lookback.minerState(miner));
              OUTCOME_TRY(seed, codec::cbor::encode(miner));
              auto post_rand{crypto::randomness::drawRandomness(
                  info.beacon().data,
                  DomainSeparationTag::WinningPoStChallengeSeed,
                  epoch,
                  seed)};
              OUTCOME_TRYA(info.sectors,
                           getSectorsForWinningPoSt(miner, state, post_rand));
              if (info.sectors.empty()) {
                return boost::none;
              }
              OUTCOME_TRY(power_state, lookback.powerState());
              OUTCOME_TRY(claim, power_state.claims.get(miner));
              info.miner_power = claim.qa_power;
              info.network_power = power_state.total_qa_power;
              OUTCOME_TRYA(info.worker, context.accountKey(state.info.worker));
              info.sector_size = state.info.sector_size;
              return info;
            }},
        .MpoolPending = {[=](auto &tipset_key)
                             -> outcome::result<std::vector<SignedMessage>> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          if (context.tipset.height > chain_store->heaviestTipset().height) {
            // tipset from future requested
            return TodoError::kError;
          }
          return mpool->pending();
        }},
        .MpoolPushMessage = {[=](auto message)
                                 -> outcome::result<SignedMessage> {
          OUTCOME_TRY(context, tipsetContext({}));
          if (message.from.isId()) {
            OUTCOME_TRYA(message.from, context.accountKey(message.from));
          }
          OUTCOME_TRYA(message.nonce, mpool->nonce(message.from));
          OUTCOME_TRY(signed_message,
                      vm::message::MessageSignerImpl{key_store}.sign(
                          message.from, message));
          OUTCOME_TRY(mpool->add(signed_message));
          return std::move(signed_message);
        }},
        .MpoolSub = {[=]() {
          auto channel{std::make_shared<Channel<MpoolUpdate>>()};
          auto cnn{std::make_shared<connection_t>()};
          *cnn = mpool->subscribe([=](auto &change) {
            if (!channel->write(change)) {
              assert(cnn->connected());
              cnn->disconnect();
            }
          });
          return Chan{std::move(channel)};
        }},
        // TODO(turuslan): FIL-165 implement method
        .NetAddrsListen = {},
        .StateAccountKey = {[=](auto &address,
                                auto &tipset_key) -> outcome::result<Address> {
          if (address.isKeyType()) {
            return address;
          }
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          return context.accountKey(address);
        }},
        .StateCall = {[=](auto &message,
                          auto &tipset_key) -> outcome::result<InvocResult> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          auto env = std::make_shared<Env>(
              std::make_shared<InvokerImpl>(), ipld, context.tipset);
          InvocResult result;
          result.message = message;
          auto maybe_result = env->applyImplicitMessage(message);
          if (maybe_result) {
            result.receipt = {VMExitCode::kOk, maybe_result.value(), 0};
          } else {
            if (isVMExitCode(maybe_result.error())) {
              auto ret_code =
                  normalizeVMExitCode(VMExitCode{maybe_result.error().value()});
              BOOST_ASSERT_MSG(ret_code,
                               "c++ actor code returned unknown error");
              result.receipt = {*ret_code, {}, 0};
            } else {
              return maybe_result.error();
            }
          }
          return result;
        }},
        .StateListMessages = {[=](auto &match, auto &tipset_key, auto to_height)
                                  -> outcome::result<std::vector<CID>> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));

          // TODO(artyom-yurin): Make sure at least one of 'to' or 'from' is
          // defined

          auto matchFunc = [&](const UnsignedMessage &message) -> bool {
            if (match.to != message.to) {
              return false;
            }

            if (match.from != message.from) {
              return false;
            }

            return true;
          };

          std::vector<CID> result;

          while (static_cast<int64_t>(context.tipset.height) >= to_height) {
            std::set<CID> visited_cid;

            auto isDuplicateMessage = [&](const CID &cid) -> bool {
              return !visited_cid.insert(cid).second;
            };

            for (const BlockHeader &block : context.tipset.blks) {
              OUTCOME_TRY(meta, ipld->getCbor<MsgMeta>(block.messages));
              OUTCOME_TRY(meta.bls_messages.visit(
                  [&](auto, auto &cid) -> outcome::result<void> {
                    OUTCOME_TRY(message, ipld->getCbor<UnsignedMessage>(cid));

                    if (!isDuplicateMessage(cid) && matchFunc(message)) {
                      result.push_back(cid);
                    }

                    return outcome::success();
                  }));
              OUTCOME_TRY(meta.secp_messages.visit(
                  [&](auto, auto &cid) -> outcome::result<void> {
                    OUTCOME_TRY(message, ipld->getCbor<SignedMessage>(cid));

                    if (!isDuplicateMessage(cid)
                        && matchFunc(message.message)) {
                      result.push_back(cid);
                    }
                    return outcome::success();
                  }));
            }

            if (context.tipset.height == 0) break;

            OUTCOME_TRY(parent_context,
                        tipsetContext(context.tipset.getParents()));

            context = std::move(parent_context);
          }

          return result;
        }},
        .StateGetActor = {[=](auto &address,
                              auto &tipset_key) -> outcome::result<Actor> {
          OUTCOME_TRY(context, tipsetContext(tipset_key, true));
          return context.state_tree.get(address);
        }},
        .StateReadState = {[=](auto &actor, auto &tipset_key)
                               -> outcome::result<ActorState> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          auto cid = actor.head;
          OUTCOME_TRY(raw, context.state_tree.getStore()->get(cid));
          return ActorState{
              .balance = actor.balance,
              .state = IpldObject{std::move(cid), std::move(raw)},
          };
        }},
        .StateGetReceipt = {[=](auto &cid, auto &tipset_key)
                                -> outcome::result<MessageReceipt> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          auto result{msg_waiter->results.find(cid)};
          if (result != msg_waiter->results.end()) {
            OUTCOME_TRY(ts, Tipset::load(*ipld, result->second.second.cids));
            if (context.tipset.height <= ts.height) {
              return result->second.first;
            }
          }
          return TodoError::kError;
        }},
        .StateListMiners = {[=](auto &tipset_key)
                                -> outcome::result<std::vector<Address>> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(power_state, context.powerState());
          return power_state.claims.keys();
        }},
        .StateListActors = {[=](auto &tipset_key)
                                -> outcome::result<std::vector<Address>> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(root, context.state_tree.flush());
          adt::Map<Actor, adt::AddressKeyer> actors{root, ipld};

          return actors.keys();
        }},
        .StateMarketBalance = {[=](auto &address, auto &tipset_key)
                                   -> outcome::result<MarketBalance> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.marketState());
          OUTCOME_TRY(id_address, context.state_tree.lookupId(address));
          OUTCOME_TRY(escrow, state.escrow_table.tryGet(id_address));
          OUTCOME_TRY(locked, state.locked_table.tryGet(id_address));
          if (!escrow) {
            escrow = 0;
          }
          if (!locked) {
            locked = 0;
          }
          return MarketBalance{*escrow, *locked};
        }},
        .StateMarketDeals = {[=](auto &tipset_key)
                                 -> outcome::result<MarketDealMap> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.marketState());
          MarketDealMap map;
          OUTCOME_TRY(state.proposals.visit([&](auto deal_id, auto &deal)
                                                -> outcome::result<void> {
            OUTCOME_TRY(deal_state, state.states.get(deal_id));
            map.emplace(std::to_string(deal_id), StorageDeal{deal, deal_state});
            return outcome::success();
          }));
          return map;
        }},
        .StateLookupID = {[=](auto &address,
                              auto &tipset_key) -> outcome::result<Address> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          return context.state_tree.lookupId(address);
        }},
        .StateMarketStorageDeal = {[=](auto deal_id, auto &tipset_key)
                                       -> outcome::result<StorageDeal> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.marketState());
          OUTCOME_TRY(deal, state.proposals.get(deal_id));
          OUTCOME_TRY(deal_state, state.states.tryGet(deal_id));
          if (!deal_state) {
            deal_state = DealState{kChainEpochUndefined,
                                   kChainEpochUndefined,
                                   kChainEpochUndefined};
          }
          return StorageDeal{deal, *deal_state};
        }},
        .StateMinerDeadlines = {[=](auto &address, auto &tipset_key)
                                    -> outcome::result<Deadlines> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.minerState(address));
          return state.getDeadlines(ipld);
        }},
        .StateMinerFaults = {[=](auto address, auto tipset_key)
                                 -> outcome::result<RleBitset> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.minerState(address));
          return state.fault_set;
        }},
        .StateMinerInfo = {[=](auto &address,
                               auto &tipset_key) -> outcome::result<MinerInfo> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(miner_state, context.minerState(address));
          return miner_state.info;
        }},
        .StateMinerPower = {[=](auto &address, auto &tipset_key)
                                -> outcome::result<MinerPower> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(power_state, context.powerState());
          OUTCOME_TRY(miner_power, power_state.claims.get(address));
          return MinerPower{
              miner_power,
              {power_state.total_raw_power, power_state.total_qa_power},
          };
        }},
        .StateMinerProvingDeadline = {[=](auto &address, auto &tipset_key)
                                          -> outcome::result<DeadlineInfo> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.minerState(address));
          return state.deadlineInfo(context.tipset.height);
        }},
        .StateMinerProvingSet =
            {[=](auto address, auto tipset_key)
                 -> outcome::result<std::vector<ChainSectorInfo>> {
              OUTCOME_TRY(context, tipsetContext(tipset_key));
              OUTCOME_TRY(state, context.minerState(address));
              std::vector<ChainSectorInfo> sectors;
              OUTCOME_TRY(state.visitProvingSet([&](auto id, auto &info) {
                sectors.emplace_back(ChainSectorInfo{info, id});
              }));
              return sectors;
            }},
        .StateMinerSectors =
            {[=](auto &address, auto &filter, auto filter_out, auto &tipset_key)
                 -> outcome::result<std::vector<ChainSectorInfo>> {
              OUTCOME_TRY(context, tipsetContext(tipset_key));
              OUTCOME_TRY(state, context.minerState(address));
              std::vector<ChainSectorInfo> sectors;
              OUTCOME_TRY(state.sectors.visit([&](auto id, auto &info) {
                if (!filter
                    || filter_out == (filter->find(id) == filter->end())) {
                  sectors.push_back({
                      .info = info,
                      .id = id,
                  });
                }
                return outcome::success();
              }));
              return sectors;
            }},
        .StateMinerSectorSize = {[=](auto address, auto tipset_key)
                                     -> outcome::result<SectorSize> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.minerState(address));
          return state.info.sector_size;
        }},
        .StateMinerWorker = {[=](auto address,
                                 auto tipset_key) -> outcome::result<Address> {
          OUTCOME_TRY(context, tipsetContext(tipset_key));
          OUTCOME_TRY(state, context.minerState(address));
          return state.info.worker;
        }},
        .StateNetworkName = {[=]() -> outcome::result<std::string> {
          OUTCOME_TRY(context, tipsetContext({chain_store->genesisCid()}));
          OUTCOME_TRY(state, context.initState());
          return state.network_name;
        }},
        .StateWaitMsg = {[=](auto &cid) -> outcome::result<Wait<MsgWait>> {
          auto channel = std::make_shared<Channel<outcome::result<MsgWait>>>();
          msg_waiter->wait(cid, [=](auto &result) {
            auto ts{Tipset::load(*ipld, result.second.cids)};
            if (ts) {
              channel->write(MsgWait{result.first, std::move(ts.value())});
            } else {
              channel->write(ts.error());
            }
          });
          return Wait{channel};
        }},
        .SyncSubmitBlock = {[=](auto block) -> outcome::result<void> {
          // TODO(turuslan): chain store must validate blocks before adding
          MsgMeta meta;
          ipld->load(meta);
          for (auto &cid : block.bls_messages) {
            OUTCOME_TRY(meta.bls_messages.append(cid));
          }
          for (auto &cid : block.secp_messages) {
            OUTCOME_TRY(meta.secp_messages.append(cid));
          }
          OUTCOME_TRY(messages, ipld->setCbor(meta));
          if (block.header.messages != messages) {
            return TodoError::kError;
          }
          OUTCOME_TRY(chain_store->addBlock(block.header));
          return outcome::success();
        }},
        .Version = {[]() {
          return VersionResult{"fuhon", 0x000300, 5};
        }},
        .WalletBalance = {[=](auto &address) -> outcome::result<TokenAmount> {
          OUTCOME_TRY(context, tipsetContext({}));
          OUTCOME_TRY(actor, context.state_tree.get(address));
          return actor.balance;
        }},
        // TODO(turuslan): FIL-165 implement method
        .WalletDefaultAddress = {},
        .WalletHas = {[=](auto address) -> outcome::result<bool> {
          if (!address.isKeyType()) {
            OUTCOME_TRY(context, tipsetContext({}));
            OUTCOME_TRYA(address, context.accountKey(address));
          }
          return key_store->has(address);
        }},
        .WalletSign = {[=](auto address,
                           auto data) -> outcome::result<Signature> {
          if (!address.isKeyType()) {
            OUTCOME_TRY(context, tipsetContext({}));
            OUTCOME_TRYA(address, context.accountKey(address));
          }
          return key_store->sign(address, data);
        }},
        .WalletVerify = {[=](auto address,
                             auto data,
                             auto signature) -> outcome::result<bool> {
          if (!address.isKeyType()) {
            OUTCOME_TRY(context, tipsetContext({}));
            OUTCOME_TRYA(address, context.accountKey(address));
          }
          return key_store->verify(address, data, signature);
        }},
        /**
         * Payment channel methods are initialized with
         * PaymentChannelManager::makeApi(Api &api)
         */
        .PaychAllocateLane = {},
        .PaychGet = {},
        .PaychVoucherAdd = {},
        .PaychVoucherCheckValid = {},
        .PaychVoucherCreate = {},
    };
  }
}  // namespace fc::api
