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

#include "globals.h"
#include "cryptonote_core/account.h"
#include "cryptonote_core/cryptonote_format_utils.h"
#include "cryptonote_core/TransactionApi.h"

#include "transfers/TransfersSynchronizer.h"
#include "transfers/BlockchainSynchronizer.h"

#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>

using namespace CryptoNote;


template <size_t size>
std::string bin2str(const std::array<uint8_t, size>& data) {
  std::string result;
  result.resize(size * 2 + 1);

  for (size_t i = 0; i < size; ++i) {
    sprintf(&result[i * 2], "%02x", data[i]);
  }

  return result;
}

class WalletObserver : public IWalletObserver {
public:
  virtual void actualBalanceUpdated(uint64_t actualBalance) {
    std::cout << "Actual balance updated = " << currency.formatAmount(actualBalance) << std::endl;
    m_actualBalance = actualBalance;
    m_sem.notify();
  }

  virtual void sendTransactionCompleted(TransactionId transactionId, std::error_code result) {
    std::cout << "Transaction sended, result = " << result << std::endl;
  }

  std::atomic<uint64_t> m_actualBalance;
  Tests::Common::Semaphore m_sem;
};

class TransactionConsumer : public IBlockchainConsumer {
public:

  TransactionConsumer() {
    syncStart.timestamp = time(nullptr);
    syncStart.height = 0;
  }

  virtual SynchronizationStart getSyncStart() override {
    return syncStart;
  }

  virtual void onBlockchainDetach(uint64_t height) override {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_transactions.lower_bound(height);
    m_transactions.erase(it, m_transactions.end());
  }

  virtual bool onNewBlocks(const CompleteBlock* blocks, uint64_t startHeight, size_t count) override {
    std::lock_guard<std::mutex> lk(m_mutex);
    for(size_t i = 0; i < count; ++i) {
      for (const auto& tx : blocks[i].transactions) {
        m_transactions[startHeight + i].insert(tx->getTransactionHash());
      }
    }
    m_cv.notify_all();
    return true;
  }

  bool waitForTransaction(const Hash& txHash) {
    std::unique_lock<std::mutex> lk(m_mutex);
    while (!hasTransaction(txHash)) {
      m_cv.wait(lk);
    }
    return true;
  }

  std::error_code onPoolUpdated(const std::vector<cryptonote::Transaction>& addedTransactions, const std::vector<crypto::hash>& deletedTransactions) override {
    //stub
    return std::error_code();
  }

  void getKnownPoolTxIds(std::vector<crypto::hash>& ids) override {
    //stub
  }

private:

  bool hasTransaction(const Hash& txHash) {
    for (const auto& kv : m_transactions) {
      if (kv.second.count(txHash) > 0)
        return true;
    }
    return false;
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::map<uint64_t, std::set<Hash>> m_transactions;
  SynchronizationStart syncStart;
};

class TransfersObserver : public ITransfersObserver {
public:
  virtual void onTransactionUpdated(ITransfersSubscription* object, const Hash& transactionHash) override {
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_transfers.push_back(transactionHash);

      auto address = epee::string_tools::pod_to_hex(object->getAddress().spendPublicKey);
      LOG_DEBUG("Transfer to " + address);
    }
    m_cv.notify_all();
  }

  bool waitTransfer() {
    std::unique_lock<std::mutex> lk(m_mutex);
    size_t prevSize = m_transfers.size();

    while (m_transfers.size() == prevSize) {
      m_cv.wait_for(lk, std::chrono::seconds(10));
    }

    return true;
  }

  bool waitTransactionTransfer(const Hash& transactionHash) {
    std::unique_lock<std::mutex> lk(m_mutex);

    while (!hasTransaction(transactionHash)) {
      m_cv.wait_for(lk, std::chrono::seconds(10));
    }

    return true;
  }

private:

  bool hasTransaction(const Hash& transactionHash) {
    return std::find(m_transfers.begin(), m_transfers.end(), transactionHash) != m_transfers.end();
  }


  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<Hash> m_transfers;
};


class AccountGroup {
public:

  AccountGroup(ITransfersSynchronizer& sync) :
    m_sync(sync) {}

  void generateAccounts(size_t count) {
    cryptonote::account_base acc;

    while (count--) {
      acc.generate();

      AccountSubscription sub;
      sub.keys = reinterpret_cast<const AccountKeys&>(acc.get_keys());
      sub.syncStart.timestamp = acc.get_createtime();
      sub.syncStart.height = 0;
      sub.transactionSpendableAge = 5;

      m_accounts.push_back(sub);
      m_addresses.push_back(currency.accountAddressAsString(acc));
    }
  }

  void subscribeAll() {
    m_observers.reset(new TransfersObserver[m_accounts.size()]);
    for (size_t i = 0; i < m_accounts.size(); ++i) {
      m_sync.addSubscription(m_accounts[i]).addObserver(&m_observers[i]);
    }
  }

  std::vector<AccountAddress> getAddresses() {
    std::vector<AccountAddress> addr;
    for (const auto& acc : m_accounts) {
      addr.push_back(acc.keys.address);
    }
    return addr;
  }

  ITransfersContainer& getTransfers(size_t idx) {
    return m_sync.getSubscription(m_accounts[idx].keys.address)->getContainer();
  }

  std::vector<AccountSubscription> m_accounts;
  std::vector<std::string> m_addresses;
  ITransfersSynchronizer& m_sync;
  std::unique_ptr<TransfersObserver[]> m_observers;
};

class MultisignatureTest : public TransfersTest {
public:

  virtual void SetUp() override {
    launchTestnet(2);
  }
};

TEST_F(TransfersTest, base) {

  uint64_t TRANSFER_AMOUNT;
  currency.parseAmount("500000.5", TRANSFER_AMOUNT);

  launchTestnet(2);

  std::unique_ptr<CryptoNote::INode> node1;
  std::unique_ptr<CryptoNote::INode> node2;

  nodeDaemons[0]->makeINode(node1);
  nodeDaemons[1]->makeINode(node2);

  cryptonote::account_base dstAcc;
  dstAcc.generate();

  AccountKeys dstKeys = reinterpret_cast<const AccountKeys&>(dstAcc.get_keys());

  BlockchainSynchronizer blockSync(*node2.get(), currency.genesisBlockHash());
  TransfersSyncronizer transferSync(currency, blockSync, *node2.get());
  TransfersObserver transferObserver;
  WalletObserver walletObserver;

  AccountSubscription sub;
  sub.syncStart.timestamp = 0;
  sub.syncStart.height = 0;
  sub.keys = dstKeys;
  sub.transactionSpendableAge = 5;

  ITransfersSubscription& transferSub = transferSync.addSubscription(sub);
  ITransfersContainer& transferContainer = transferSub.getContainer();
  transferSub.addObserver(&transferObserver);

  std::unique_ptr<IWallet> wallet1;

  makeWallet(wallet1, node1);
  mineBlock(wallet1);

  wallet1->addObserver(&walletObserver);

  startMining(1);

  while (wallet1->actualBalance() < TRANSFER_AMOUNT) {
    walletObserver.m_sem.wait();
  }

  // start syncing and wait for a transfer
  auto waitFuture = std::async(std::launch::async, [&transferObserver] { return transferObserver.waitTransfer(); });
  blockSync.start();

  Transfer transfer;
  transfer.address = currency.accountAddressAsString(dstAcc);
  transfer.amount = TRANSFER_AMOUNT;

  wallet1->sendTransaction(transfer, currency.minimumFee());

  auto result = waitFuture.get();

  std::cout << "Received transfer: " << currency.formatAmount(transferContainer.balance(ITransfersContainer::IncludeAll)) << std::endl;

  ASSERT_EQ(TRANSFER_AMOUNT, transferContainer.balance(ITransfersContainer::IncludeAll));
 
  auto BACK_TRANSFER = TRANSFER_AMOUNT / 2;

  stopMining();
  blockSync.stop();
}


std::unique_ptr<ITransaction> createTransferToMultisignature(
  ITransfersContainer& tc, // money source
  uint64_t amount,
  uint64_t fee,
  const AccountKeys& senderKeys,
  const std::vector<AccountAddress>& recipients,
  uint32_t requiredSignatures) {

  std::vector<TransactionOutputInformation> transfers;
  tc.getOutputs(transfers, ITransfersContainer::IncludeAllUnlocked | ITransfersContainer::IncludeStateSoftLocked);

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

  // output to receiver
  tx->addOutput(amount, recipients, requiredSignatures);

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

std::error_code submitTransaction(INode& node, ITransactionReader& tx) {
  auto data = tx.getTransactionData();

  cryptonote::blobdata txblob(data.data(), data.data() + data.size());
  cryptonote::Transaction outTx;
  cryptonote::parse_and_validate_tx_from_blob(txblob, outTx);

  LOG_DEBUG("Submitting transaction " + bin2str(tx.getTransactionHash()));

  std::promise<std::error_code> result;
  node.relayTransaction(outTx, [&result](std::error_code ec) { result.set_value(ec); });
  auto err = result.get_future().get();

  if (err) {
    LOG_DEBUG("Error: " + err.message());
  } else {
    LOG_DEBUG("Submitted successfully");
  }

  return err;
}


std::unique_ptr<ITransaction> createTransferFromMultisignature(
  AccountGroup& consilium, const AccountAddress& receiver, const Hash& txHash, uint64_t amount, uint64_t fee) {

  auto& tc = consilium.getTransfers(0);

  std::vector<TransactionOutputInformation> transfers = tc.getTransactionOutputs(txHash,
    ITransfersContainer::IncludeTypeMultisignature |
    ITransfersContainer::IncludeStateSoftLocked |
    ITransfersContainer::IncludeStateUnlocked);

  const TransactionOutputInformation& out = transfers[0];

  auto tx = createTransaction();

  TransactionTypes::InputMultisignature msigInput;

  msigInput.amount = out.amount;
  msigInput.outputIndex = out.globalOutputIndex;
  msigInput.signatures = out.requiredSignatures;

  tx->addInput(msigInput);
  tx->addOutput(amount, receiver);

  uint64_t change = out.amount - amount - fee;

  tx->addOutput(change, consilium.getAddresses(), out.requiredSignatures);

  for (size_t i = 0; i < out.requiredSignatures; ++i) {
    tx->signInputMultisignature(0, out.transactionPublicKey, out.outputInTransaction, consilium.m_accounts[i].keys);
  }

  return tx;
}

TEST_F(MultisignatureTest, createMulitisignatureTransaction) {

  std::unique_ptr<CryptoNote::INode> node1;
  std::unique_ptr<CryptoNote::INode> node2;

  nodeDaemons[0]->makeINode(node1);
  nodeDaemons[1]->makeINode(node2);

  BlockchainSynchronizer blockSync(*node2.get(), currency.genesisBlockHash());
  TransfersSyncronizer transferSync(currency, blockSync, *node2.get());
  
  // add transaction collector
  TransactionConsumer txConsumer;
  blockSync.addConsumer(&txConsumer);

  AccountGroup sender(transferSync);
  AccountGroup consilium(transferSync);

  sender.generateAccounts(1);
  sender.subscribeAll();

  consilium.generateAccounts(3);
  consilium.subscribeAll();

  auto senderSubscription = transferSync.getSubscription(sender.m_accounts[0].keys.address);
  auto& senderContainer = senderSubscription->getContainer();

  blockSync.start();

  // start mining for sender
  nodeDaemons[0]->startMining(1, sender.m_addresses[0]);
  
  // wait for incoming transfer
  while (senderContainer.balance() == 0) {
    sender.m_observers[0].waitTransfer();

    auto unlockedBalance = senderContainer.balance(ITransfersContainer::IncludeAllUnlocked | ITransfersContainer::IncludeStateSoftLocked);
    auto totalBalance = senderContainer.balance(ITransfersContainer::IncludeAll);

    LOG_DEBUG("Balance: " + currency.formatAmount(unlockedBalance) + " (" + currency.formatAmount(totalBalance) + ")");
  }

  uint64_t fundBalance = 0;

  for (int iteration = 1; iteration <= 3; ++iteration) {
    LOG_DEBUG("***** Iteration " + std::to_string(iteration) + " ******");

    auto sendAmount = senderContainer.balance() / 2;

    LOG_DEBUG("Creating transaction with amount = " + currency.formatAmount(sendAmount));

    auto tx2msig = createTransferToMultisignature(
      senderContainer, sendAmount, currency.minimumFee(), sender.m_accounts[0].keys, consilium.getAddresses(), 3);

    auto txHash = tx2msig->getTransactionHash();
    auto err = submitTransaction(*node2, *tx2msig);
    ASSERT_EQ(std::error_code(), err);

    LOG_DEBUG("Waiting for transaction to be included in block...");
    txConsumer.waitForTransaction(txHash);

    LOG_DEBUG("Transaction in blockchain, waiting for observers to receive transaction...");

    uint64_t expectedFundBalance = fundBalance + sendAmount;

    // wait for consilium to receive the transfer
    for (size_t i = 0; i < consilium.m_accounts.size(); ++i) {
      auto& observer = consilium.m_observers[i];
      observer.waitTransactionTransfer(txHash);

      auto sub = transferSync.getSubscription(consilium.m_accounts[i].keys.address);
      ASSERT_TRUE(sub != nullptr);
      ASSERT_EQ(expectedFundBalance, sub->getContainer().balance(
        ITransfersContainer::IncludeStateAll | ITransfersContainer::IncludeTypeMultisignature));
    }

    LOG_DEBUG("Creating transaction to spend multisignature output");

    uint64_t returnAmount = sendAmount / 2;

    auto spendMsigTx = createTransferFromMultisignature(
      consilium, sender.m_accounts[0].keys.address, txHash, returnAmount, currency.minimumFee());

    auto spendMsigTxHash = spendMsigTx->getTransactionHash();

    err = submitTransaction(*node2, *spendMsigTx);

    ASSERT_EQ(std::error_code(), err);

    LOG_DEBUG("Waiting for transaction to be included in block...");
    txConsumer.waitForTransaction(spendMsigTxHash);

    LOG_DEBUG("Checking left balances");
    // check that outputs were correctly marked as spent
    uint64_t leftAmount = expectedFundBalance - returnAmount - currency.minimumFee();
    for (size_t i = 0; i < consilium.m_accounts.size(); ++i) {
      auto& observer = consilium.m_observers[i];
      observer.waitTransactionTransfer(spendMsigTxHash);
      ASSERT_EQ(leftAmount, consilium.getTransfers(i).balance(ITransfersContainer::IncludeAll));
    }

    fundBalance = leftAmount;
  }

  stopMining();
  blockSync.stop();

  LOG_DEBUG("Success!!!");
}
