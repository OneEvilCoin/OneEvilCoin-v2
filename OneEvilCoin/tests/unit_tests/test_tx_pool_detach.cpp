// Copyright (c) 2012-2014, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "gtest/gtest.h"

#include "transfers/BlockchainSynchronizer.h"
#include "transfers/TransfersSynchronizer.h"

#include "INodeStubs.h"
#include "TestBlockchainGenerator.h"
#include "TransactionApiHelpers.h"
#include "cryptonote_core/TransactionApi.h"

#include "wallet/Wallet.h"

#include <boost/scoped_array.hpp>

#include <future>
#include <algorithm>

using namespace CryptoNote;

/*
class TransfersObserver : public ITransfersObserver {
public:

  virtual void onTransactionUpdated(ITransfersSubscription* object, const Hash& transactionHash,
    uint64_t amountIn, uint64_t amountOut) override {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_transfers.push_back(std::make_pair(transactionHash, amountIn - amountOut));
  }

  std::vector<std::pair<Hash, int64_t>> m_transfers;
  std::mutex m_mutex;
}; */


class INodeStubWithPoolTx : public INodeTrivialRefreshStub {
public:
  INodeStubWithPoolTx(TestBlockchainGenerator& generator) : INodeTrivialRefreshStub(generator), detached(false) {}

  void relayTransaction(const cryptonote::Transaction& transaction, const Callback& callback) override {
    relayedTxs.push_back(std::make_pair(this->getLastLocalBlockHeight(), transaction));
    INodeTrivialRefreshStub::relayTransaction(transaction, callback);
  }

  void startAlternativeChain(uint64_t height) override {
    INodeTrivialRefreshStub::startAlternativeChain(height);
    detachHeight = height;
    detached = true;
  }


  void getPoolSymmetricDifference(std::vector<crypto::hash>&& known_pool_tx_ids, crypto::hash known_block_id, bool& is_bc_actual, std::vector<cryptonote::Transaction>& new_txs, std::vector<crypto::hash>& deleted_tx_ids, const Callback& callback) override 
  {
    std::sort(relayedTxs.begin(), relayedTxs.end(), [](const std::pair<uint64_t, cryptonote::Transaction>& val1, const std::pair<uint64_t, cryptonote::Transaction>& val2)->bool {return val1.first < val2.first; });
    is_bc_actual = true;
    
    if (detached) {
      size_t i = 0;
      for (; i < relayedTxs.size(); ++i) {
        if (relayedTxs[i].first >= detachHeight) {
          break;
        }
      }

      for (; i < relayedTxs.size(); ++i) {
        new_txs.push_back(relayedTxs[i].second);
      }
    }

    callback(std::error_code()); 
  };

  
  std::vector<std::pair<uint64_t, cryptonote::Transaction>> relayedTxs;
  uint64_t detachHeight;
  bool detached;

};


class DetachTest : public ::testing::Test, public IBlockchainSynchronizerObserver {
public:

  DetachTest() :
    m_currency(cryptonote::CurrencyBuilder().currency()),
    generator(m_currency),
    m_node(generator),
    m_sync(m_node, m_currency.genesisBlockHash()),
    m_transfersSync(m_currency, m_sync, m_node) {
  }

  void addAccounts(size_t count) {
    while (count--) {
      m_accounts.push_back(generateAccountKeys());
    }
  }

  void addMinerAccount() {
    m_accounts.push_back(reinterpret_cast<const AccountKeys&>(generator.getMinerAccount()));
  }

  AccountSubscription createSubscription(size_t acc, uint64_t timestamp = 0) {
    const auto& keys = m_accounts[acc];
    AccountSubscription sub;
    sub.keys = keys;
    sub.syncStart.timestamp = timestamp;
    sub.syncStart.height = 0;
    sub.transactionSpendableAge = 5;
    return sub;
  }

  void subscribeAccounts() {

   // m_transferObservers.reset(new TransfersObserver[m_accounts.size()]);

    for (size_t i = 0; i < m_accounts.size(); ++i) {
      m_subscriptions.push_back(&m_transfersSync.addSubscription(createSubscription(i)));
      //m_subscriptions.back()->addObserver(&m_transferObservers[i]);
    }
  }

  void generateMoneyForAccount(size_t idx) {
    generator.getBlockRewardForAddress(
      reinterpret_cast<const cryptonote::AccountPublicAddress&>(m_accounts[idx].address));
  }

  std::error_code submitTransaction(ITransactionReader& tx) {
    auto data = tx.getTransactionData();

    cryptonote::blobdata txblob(data.data(), data.data() + data.size());
    cryptonote::Transaction outTx;
    cryptonote::parse_and_validate_tx_from_blob(txblob, outTx);

    std::promise<std::error_code> result;
    m_node.relayTransaction(outTx, [&result](std::error_code ec) { result.set_value(ec); });
    return result.get_future().get();
  }

  void synchronizationCompleted(std::error_code result) override {
    decltype(syncCompleted) detachedPromise = std::move(syncCompleted);
    detachedPromise.set_value(result);
  }


protected:
  std::vector<AccountKeys> m_accounts;
  std::vector<ITransfersSubscription*> m_subscriptions;

  cryptonote::Currency m_currency;
  TestBlockchainGenerator generator;
  INodeStubWithPoolTx m_node;
  BlockchainSynchronizer m_sync;
  TransfersSyncronizer m_transfersSync;

  std::promise<std::error_code> syncCompleted;
  std::future<std::error_code> syncCompletedFuture;

};


namespace CryptoNote {
inline bool operator == (const TransactionOutputInformation& t1, const TransactionOutputInformation& t2) {
  return
    t1.type == t2.type &&
    t1.amount == t2.amount &&
    t1.outputInTransaction == t2.outputInTransaction &&
    t1.transactionPublicKey == t2.transactionPublicKey;
}
}

namespace {
  std::unique_ptr<ITransaction> createMoneyTransfer(
    uint64_t amount,
    uint64_t fee,
    const AccountKeys& senderKeys,
    const AccountAddress& reciever,
    ITransfersContainer& tc) {

    std::vector<TransactionOutputInformation> transfers;
    tc.getOutputs(transfers, ITransfersContainer::IncludeAllUnlocked);

    auto tx = createTransaction();

    std::vector<std::pair<TransactionTypes::InputKeyInfo, KeyPair>> inputs;

    uint64_t foundMoney = 0;

    for (const auto& t : transfers) {
      TransactionTypes::InputKeyInfo info;

      info.amount = t.amount;

      TransactionTypes::GlobalOutput globalOut;
      globalOut.outputIndex = t.globalOutputIndex;
      globalOut.targetKey = t.outputKey;
      info.outputs.push_back(globalOut);

      info.realOutput.outputInTransaction = t.outputInTransaction;
      info.realOutput.transactionIndex = 0;
      info.realOutput.transactionPublicKey = t.transactionPublicKey;

      KeyPair kp;
      tx->addInput(senderKeys, info, kp);

      inputs.push_back(std::make_pair(info, kp));

      foundMoney += info.amount;

      if (foundMoney >= amount + fee) {
        break;
      }
    }

    // output to reciever
    tx->addOutput(amount, reciever);
    // change
    uint64_t change = foundMoney - amount - fee;
    if (change) {
      tx->addOutput(change, senderKeys.address);
    }

    for (size_t inputIdx = 0; inputIdx < inputs.size(); ++inputIdx) {
      tx->signInputKey(inputIdx, inputs[inputIdx].first, inputs[inputIdx].second);
    }

    return tx;
  }
}


TEST_F(DetachTest, testBlockchainDetach) {
  uint64_t sendAmount = 70000000000000;
  auto fee = m_currency.minimumFee();

  addMinerAccount();
  addAccounts(2);
  subscribeAccounts();

  generator.generateEmptyBlocks(20);
  
  syncCompleted = std::promise<std::error_code>();
  syncCompletedFuture = syncCompleted.get_future();
  m_sync.addObserver(this);
  m_sync.start();
  syncCompletedFuture.get();
  m_sync.removeObserver(this);

  auto& tc = m_subscriptions[0]->getContainer();
  auto prevBalance = tc.balance();

  ASSERT_LE(sendAmount, tc.balance(ITransfersContainer::IncludeAllUnlocked));

  auto tx = createMoneyTransfer(sendAmount, fee, m_accounts[0], m_accounts[1].address, tc);
  submitTransaction(*tx);

  syncCompleted = std::promise<std::error_code>();
  syncCompletedFuture = syncCompleted.get_future();
  m_sync.addObserver(this);
  m_node.updateObservers();
  syncCompletedFuture.get();
  m_sync.removeObserver(this);
  auto& tc2 = m_subscriptions[1]->getContainer();

  ASSERT_EQ(sendAmount, tc2.balance(ITransfersContainer::IncludeAll));
  ASSERT_EQ(0, tc2.balance(ITransfersContainer::IncludeAllUnlocked));
  ASSERT_EQ(1, tc2.transactionsCount());

  std::vector<crypto::hash> unconfirmed;
  tc2.getUnconfirmedTransactions(unconfirmed);  
  ASSERT_EQ(0, unconfirmed.size());

  m_node.startAlternativeChain(m_node.getLastLocalBlockHeight() - 1);
  generator.generateEmptyBlocks(2);

  syncCompleted = std::promise<std::error_code>();
  syncCompletedFuture = syncCompleted.get_future();
  m_sync.addObserver(this);
  m_node.updateObservers();
  syncCompletedFuture.get();
  m_sync.removeObserver(this);
  auto& tc3 = m_subscriptions[1]->getContainer();

  ASSERT_EQ(sendAmount, tc3.balance(ITransfersContainer::IncludeAll));
  ASSERT_EQ(0, tc3.balance(ITransfersContainer::IncludeAllUnlocked));
  ASSERT_EQ(1, tc3.transactionsCount());

  tc3.getUnconfirmedTransactions(unconfirmed);
  ASSERT_EQ(1, unconfirmed.size());
  ASSERT_EQ(reinterpret_cast<const Hash&>(unconfirmed[0]), tx->getTransactionHash());
  m_sync.stop();
}


struct CompletionWalletObserver : public IWalletObserver {
  virtual void synchronizationCompleted(std::error_code result) override {
    syncCompleted.set_value(result);
  }

  std::promise<std::error_code> syncCompleted;
  std::future<std::error_code> syncCompletedFuture;
};


struct WaitForExternalTransactionObserver : public CryptoNote::IWalletObserver {
public:
  WaitForExternalTransactionObserver() {}
  std::promise<CryptoNote::TransactionId> promise;

  virtual void externalTransactionCreated(CryptoNote::TransactionId transactionId) override {
    promise.set_value(transactionId);
  }

};

TEST_F(DetachTest, testDetachWithWallet) {
  auto fee = m_currency.minimumFee();

  generator.generateEmptyBlocks(5);
  Wallet Alice(m_currency, m_node);
  Wallet Bob(m_currency, m_node);

  CompletionWalletObserver AliceCompleted, BobCompleted;
  AliceCompleted.syncCompleted = std::promise<std::error_code>();
  AliceCompleted.syncCompletedFuture = AliceCompleted.syncCompleted.get_future();
  BobCompleted.syncCompleted = std::promise<std::error_code>();
  BobCompleted.syncCompletedFuture = BobCompleted.syncCompleted.get_future();
  Alice.addObserver(&AliceCompleted);
  Bob.addObserver(&BobCompleted);
  Alice.initAndGenerate("pass");
  Bob.initAndGenerate("pass");
  AliceCompleted.syncCompletedFuture.get();
  BobCompleted.syncCompletedFuture.get();
  Alice.removeObserver(&AliceCompleted);
  Bob.removeObserver(&BobCompleted);


  cryptonote::AccountPublicAddress AliceAddr;
  WalletAccountKeys AliceKeys;
  Alice.getAccountKeys(AliceKeys);
  AliceAddr.m_spendPublicKey = *reinterpret_cast<crypto::public_key*>(&AliceKeys.spendPublicKey);
  AliceAddr.m_viewPublicKey = *reinterpret_cast<crypto::public_key*>(&AliceKeys.viewPublicKey);
  generator.getBlockRewardForAddress(AliceAddr);


  generator.generateEmptyBlocks(10);

  AliceCompleted.syncCompleted = std::promise<std::error_code>();
  AliceCompleted.syncCompletedFuture = AliceCompleted.syncCompleted.get_future();
  BobCompleted.syncCompleted = std::promise<std::error_code>();
  BobCompleted.syncCompletedFuture = BobCompleted.syncCompleted.get_future();
  Alice.addObserver(&AliceCompleted);
  Bob.addObserver(&BobCompleted);

  m_node.updateObservers();

  AliceCompleted.syncCompletedFuture.get();
  BobCompleted.syncCompletedFuture.get();
  Alice.removeObserver(&AliceCompleted);
  Bob.removeObserver(&BobCompleted);


  ASSERT_EQ(0, Alice.pendingBalance());
  ASSERT_NE(0, Alice.actualBalance());

  CryptoNote::Transfer tr;

  tr.amount = Alice.actualBalance() / 2;
  tr.address = Bob.getAddress();

  Alice.sendTransaction(tr, fee);

  WaitForExternalTransactionObserver etxo;
  auto externalTxFuture = etxo.promise.get_future();
  Bob.addObserver(&etxo);
  AliceCompleted.syncCompleted = std::promise<std::error_code>();
  AliceCompleted.syncCompletedFuture = AliceCompleted.syncCompleted.get_future();
  BobCompleted.syncCompleted = std::promise<std::error_code>();
  BobCompleted.syncCompletedFuture = BobCompleted.syncCompleted.get_future();
  Alice.addObserver(&AliceCompleted);
  Bob.addObserver(&BobCompleted);

  m_node.updateObservers();

  AliceCompleted.syncCompletedFuture.get();
  BobCompleted.syncCompletedFuture.get();
  Alice.removeObserver(&AliceCompleted);
  Bob.removeObserver(&BobCompleted);

  auto txId = externalTxFuture.get();
  Bob.removeObserver(&etxo);

  TransactionInfo txInfo;

  Bob.getTransaction(txId, txInfo);


  ASSERT_EQ(txInfo.blockHeight, m_node.getLastLocalBlockHeight());
  ASSERT_EQ(txInfo.totalAmount, tr.amount);

  ASSERT_EQ(Bob.pendingBalance(), 0);
  ASSERT_EQ(Bob.actualBalance(), tr.amount);

  m_node.startAlternativeChain(m_node.getLastLocalBlockHeight() - 1);
  generator.generateEmptyBlocks(2);

  //sync Bob
  AliceCompleted.syncCompleted = std::promise<std::error_code>();
  AliceCompleted.syncCompletedFuture = AliceCompleted.syncCompleted.get_future();
  BobCompleted.syncCompleted = std::promise<std::error_code>();
  BobCompleted.syncCompletedFuture = BobCompleted.syncCompleted.get_future();
  Alice.addObserver(&AliceCompleted);
  Bob.addObserver(&BobCompleted);

  m_node.updateObservers();

  AliceCompleted.syncCompletedFuture.get();
  BobCompleted.syncCompletedFuture.get();
  Alice.removeObserver(&AliceCompleted);
  Bob.removeObserver(&BobCompleted);

  Bob.getTransaction(txId, txInfo);
  ASSERT_EQ(txInfo.blockHeight, UNCONFIRMED_TRANSACTION_HEIGHT);
  ASSERT_EQ(txInfo.totalAmount, tr.amount);

  ASSERT_EQ(Bob.pendingBalance(), tr.amount);
  ASSERT_EQ(Bob.actualBalance(), 0);
}
