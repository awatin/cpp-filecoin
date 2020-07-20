/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "primitives/tipset/tipset.hpp"

#include "common/logger.hpp"
#include "primitives/address/address_codec.hpp"
#include "primitives/cid/cid_of_cbor.hpp"

OUTCOME_CPP_DEFINE_CATEGORY(fc::primitives::tipset, TipsetError, e) {
  using fc::primitives::tipset::TipsetError;
  switch (e) {
    case (TipsetError::NO_BLOCKS):
      return "No blocks to create tipset";
    case TipsetError::MISMATCHING_HEIGHTS:
      return "Cannot create tipset, mismatching blocks heights";
    case TipsetError::MISMATCHING_PARENTS:
      return "Cannot create tipset, mismatching block parents";
    case TipsetError::TICKET_HAS_NO_VALUE:
      return "An optional ticket is not initialized";
    case TipsetError::TICKETS_COLLISION:
      return "Duplicate tickets in tipset";
    case TipsetError::BLOCK_ORDER_FAILURE:
      return "Wrong order of blocks in tipset";
  }
  return "Unknown tipset error";
}

namespace fc::primitives::tipset {

  outcome::result<void> MessageVisitor::visit(const BlockHeader &block,
                                              const Visitor &visitor) {
    auto onMessage = [&](auto bls, auto &cid) -> outcome::result<void> {
      if (visited.insert(cid).second) {
        OUTCOME_TRY(visitor(visited.size() - 1, bls, cid));
      }
      return outcome::success();
    };
    OUTCOME_TRY(meta, ipld->getCbor<block::MsgMeta>(block.messages));
    OUTCOME_TRY(meta.bls_messages.visit(
        [&](auto, auto &cid) { return onMessage(true, cid); }));
    OUTCOME_TRY(meta.secp_messages.visit(
        [&](auto, auto &cid) { return onMessage(false, cid); }));
    return outcome::success();
  }

  outcome::result<void> TipsetCreator::canExpandTipset(
      const block::BlockHeader &hdr) const {
    if (blks_.empty()) {
      return outcome::success();
    }

    if (!hdr.ticket.has_value()) {
      return TipsetError::TICKET_HAS_NO_VALUE;
    }

    const auto &first_block = blks_[0];

    if (hdr.height != first_block.height) {
      return TipsetError::MISMATCHING_HEIGHTS;
    }

    if (hdr.parents != first_block.parents) {
      return TipsetError::MISMATCHING_PARENTS;
    }

    return outcome::success();
  }

  outcome::result<void> TipsetCreator::expandTipset(block::BlockHeader hdr) {
    OUTCOME_TRY(cid, fc::primitives::cid::getCidOfCbor(hdr));
    return expandTipset(std::move(cid), std::move(hdr));
  }

  outcome::result<void> TipsetCreator::expandTipset(CID cid,
                                                    block::BlockHeader hdr) {
    // must be called prior to expand()
    assert(canExpandTipset(hdr));

    constexpr auto kReserveSize = 5;

    if (blks_.empty()) {
      blks_.reserve(kReserveSize);
      blks_.emplace_back(std::move(hdr));
      cids_.reserve(kReserveSize);
      cids_.emplace_back(std::move(cid));
      return outcome::success();
    }

    auto it = blks_.begin();
    const auto &ticket = hdr.ticket.value();
    size_t idx = 0;
    for (auto e = blks_.end(); it != e; ++it, ++idx) {
      int c = ticket::compare(ticket, it->ticket.value());
      if (c == 0) {
        return TipsetError::TICKETS_COLLISION;
      }
      if (c < 0) {
        continue;
      }
      break;
    }

    if (++it == blks_.end()) {
      // most likely they come in proper order
      blks_.push_back(std::move(hdr));
      cids_.push_back(std::move(cid));
    } else {
      blks_.insert(blks_.begin() + idx, std::move(hdr));
      cids_.insert(cids_.begin() + idx, std::move(cid));
    }

    return outcome::success();
  }

  Tipset TipsetCreator::getTipset(bool clear) {
    if (blks_.empty()) {
      return Tipset{};
    }
    if (clear) {
      OUTCOME_EXCEPT(key, TipsetKey::create(std::move(cids_)));
      return Tipset{std::move(key), std::move(blks_)};
    }

    // make copy, don't erase
    OUTCOME_EXCEPT(key, TipsetKey::create(cids_));
    return Tipset{key, blks_};
  }

  void TipsetCreator::clear() {
    blks_.clear();
    cids_.clear();
  }

  uint64_t TipsetCreator::height() const {
    return blks_.empty() ? 0 : blks_[0].height;
  }

  outcome::result<Tipset> Tipset::create(const TipsetHash &hash,
                                         BlocksAvailable blocks) {
    TipsetCreator creator;

    for (auto &b : blocks) {
      if (!b.has_value()) {
        return TipsetError::NO_BLOCKS;
      }

      auto &hdr = b.value();
      OUTCOME_TRY(creator.canExpandTipset(hdr));
      OUTCOME_TRY(creator.expandTipset(std::move(hdr)));
    }

    Tipset tipset = creator.getTipset(true);
    if (tipset.key.hash() != hash) {
      return TipsetError::BLOCK_ORDER_FAILURE;
    }

    return std::move(tipset);
  }

  outcome::result<Tipset> Tipset::create(
      std::vector<block::BlockHeader> blocks) {
    TipsetCreator creator;

    for (auto &hdr : blocks) {
      OUTCOME_TRY(creator.canExpandTipset(hdr));
      OUTCOME_TRY(creator.expandTipset(std::move(hdr)));
    }

    return creator.getTipset(true);
  }

  outcome::result<Tipset> Tipset::load(Ipld &ipld,
                                       const std::vector<CID> &cids) {
    std::vector<BlockHeader> blocks;
    blocks.reserve(cids.size());
    for (auto &cid : cids) {
      OUTCOME_TRY(block, ipld.getCbor<BlockHeader>(cid));
      blocks.emplace_back(std::move(block));
    }
    return create(std::move(blocks));
  }

  outcome::result<Tipset> Tipset::loadParent(Ipld &ipld) const {
    return load(ipld, blks[0].parents);
  }

  outcome::result<void> Tipset::visitMessages(
      IpldPtr ipld, const MessageVisitor::Visitor &visitor) const {
    MessageVisitor message_visitor{ipld};
    for (auto &block : blks) {
      OUTCOME_TRY(message_visitor.visit(block, visitor));
    }
    return outcome::success();
  }

  outcome::result<TipsetKey> Tipset::getParents() const {
    return blks.empty() ? TipsetKey() : TipsetKey::create(blks[0].parents);
  }

  uint64_t Tipset::getMinTimestamp() const {
    if (blks.empty()) return 0;
    return std::min_element(blks.begin(),
                            blks.end(),
                            [](const auto &b1, const auto &b2) -> bool {
                              return b1.timestamp < b2.timestamp;
                            })
        ->timestamp;
  }

  const block::BlockHeader &Tipset::getMinTicketBlock() const {
    // i believe that Tipset::create sorts them
    return blks[0];
  }

  const CID &Tipset::getParentStateRoot() const {
    return blks[0].parent_state_root;
  }

  const BigInt &Tipset::getParentWeight() const {
    static const BigInt zero;
    return blks.empty() ? zero : blks[0].parent_weight;
  }

  const CID &Tipset::getParentMessageReceipts() const {
    return blks[0].parent_message_receipts;
  }

  uint64_t Tipset::height() const {
    return blks.empty() ? 0 : blks[0].height;
  }

  bool Tipset::contains(const CID &cid) const {
    const auto &cids = key.cids();
    return std::find(cids.begin(), cids.end(), cid) != std::end(cids);
  }

  bool operator==(const Tipset &lhs, const Tipset &rhs) {
    if (lhs.blks.size() != rhs.blks.size()) return false;
    return std::equal(lhs.blks.begin(), lhs.blks.end(), rhs.blks.begin());
  }

  bool operator!=(const Tipset &l, const Tipset &r) {
    return !(l == r);
  }
}  // namespace fc::primitives::tipset

namespace fc::codec::cbor {

  namespace {
    struct TipsetDecodeCandidate {
      std::vector<CID> cids;
      std::vector<fc::primitives::block::BlockHeader> blks;
      uint64_t height;
    };

    CBOR_TUPLE(TipsetDecodeCandidate, cids, blks, height);

  }  // namespace

  template <>
  outcome::result<fc::primitives::tipset::Tipset>
  decode<fc::primitives::tipset::Tipset>(gsl::span<const uint8_t> input) {
    using namespace fc::primitives::tipset;

    OUTCOME_TRY(decoded, decode<TipsetDecodeCandidate>(input));
    if (decoded.blks.empty() && decoded.height != 0) {
      return TipsetError::MISMATCHING_HEIGHTS;
    }
    OUTCOME_TRY(tipset, Tipset::create(std::move(decoded.blks)));
    if (tipset.key.cids() != decoded.cids) {
      return TipsetError::BLOCK_ORDER_FAILURE;
    }
    return std::move(tipset);
  }

}  // namespace fc::codec::cbor
