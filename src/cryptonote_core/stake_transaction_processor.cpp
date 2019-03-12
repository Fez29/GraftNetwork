#include <string_tools.h>

#include "stake_transaction_processor.h"
#include "../graft_rta_config.h"

using namespace cryptonote;

namespace
{

const char* STAKE_TRANSACTION_STORAGE_FILE_NAME = "stake_transactions.v2.bin";
const char* BLOCKCHAIN_BASED_LIST_FILE_NAME     = "blockchain_based_list.v3.bin";

}

bool stake_transaction::is_valid(uint64_t block_index) const
{
  uint64_t stake_first_valid_block = block_height + config::graft::STAKE_VALIDATION_PERIOD,
           stake_last_valid_block  = block_height + unlock_time + config::graft::TRUSTED_RESTAKING_PERIOD;

  if (block_index < stake_first_valid_block)
    return false;

  if (block_index >= stake_last_valid_block)
    return false;

  return true;
}

StakeTransactionProcessor::StakeTransactionProcessor(Blockchain& blockchain)
  : m_blockchain(blockchain)
  , m_stakes_need_update(true)
  , m_blockchain_based_list_need_update(true)
{
}

const supernode_stake* StakeTransactionProcessor::find_supernode_stake(uint64_t block_number, const std::string& supernode_public_id) const
{
  if (!m_storage)
    return nullptr;

  return m_storage->find_supernode_stake(block_number, supernode_public_id);
}

namespace
{

uint64_t get_transaction_amount(const transaction& tx, const account_public_address& address, const crypto::secret_key& tx_key)
{
  crypto::key_derivation derivation;

  if (!crypto::generate_key_derivation(address.m_view_public_key, tx_key, derivation))
  {
    MCLOG(el::Level::Warning, "global", "failed to generate key derivation from supplied parameters");
    return 0;
  }

  uint64_t received = 0;

  for (size_t n = 0; n < tx.vout.size(); ++n)
  {
    if (typeid(cryptonote::txout_to_key) != tx.vout[n].target.type())
      continue;

    const cryptonote::txout_to_key tx_out_to_key = boost::get<cryptonote::txout_to_key>(tx.vout[n].target);

    crypto::public_key pubkey;
    derive_public_key(derivation, n, address.m_spend_public_key, pubkey);

    if (pubkey == tx_out_to_key.key)
    {
      uint64_t amount;
      if (tx.version == 1)
      {
        amount = tx.vout[n].amount;
      }
      else
      {
        try
        {
          rct::key Ctmp;
          //rct::key amount_key = rct::hash_to_scalar(rct::scalarmultKey(rct::pk2rct(address.m_view_public_key), rct::sk2rct(tx_key)));
          crypto::key_derivation derivation;
          bool r = crypto::generate_key_derivation(address.m_view_public_key, tx_key, derivation);
          if (!r)
          {
            LOG_ERROR("Failed to generate key derivation to decode rct output " << n);
            amount = 0;
          }
          else
          {
            crypto::secret_key scalar1;
            crypto::derivation_to_scalar(derivation, n, scalar1);
            rct::ecdhTuple ecdh_info = tx.rct_signatures.ecdhInfo[n];
            rct::ecdhDecode(ecdh_info, rct::sk2rct(scalar1));
            rct::key C = tx.rct_signatures.outPk[n].mask;
            rct::addKeys2(Ctmp, ecdh_info.mask, ecdh_info.amount, rct::H);
            if (rct::equalKeys(C, Ctmp))
              amount = rct::h2d(ecdh_info.amount);
            else
              amount = 0;
          }
        }
        catch (...) { amount = 0; }
      }
      received += amount;
    }
  }

  return received;
}

}

void StakeTransactionProcessor::init_storages(const std::string& config_dir)
{
  if (m_storage || m_blockchain_based_list)
    throw std::runtime_error("StakeTransactionProcessor storages have been already initialized");

  m_storage.reset(new StakeTransactionStorage(config_dir + "/" + STAKE_TRANSACTION_STORAGE_FILE_NAME));
  m_blockchain_based_list.reset(new BlockchainBasedList(config_dir + "/" + BLOCKCHAIN_BASED_LIST_FILE_NAME));
}

void StakeTransactionProcessor::process_block_stake_transaction(uint64_t block_index, const block& block, const crypto::hash& block_hash, bool update_storage)
{
  if (block_index <= m_storage->get_last_processed_block_index())
    return;

  BlockchainDB& db = m_blockchain.get_db();

    //analyze block transactions and add new stake transactions if exist

  stake_transaction stake_tx;

  for (const crypto::hash& tx_hash : block.tx_hashes)
  {
    try
    {
      const transaction& tx = db.get_tx(tx_hash);

      if (!get_graft_stake_tx_extra_from_extra(tx, stake_tx.supernode_public_id, stake_tx.supernode_public_address, stake_tx.supernode_signature, stake_tx.tx_secret_key))
        continue;

      crypto::public_key W;
      if (!epee::string_tools::hex_to_pod(stake_tx.supernode_public_id, W) || !check_key(W))
      {
        MCLOG(el::Level::Warning, "global", "Ignore stake transaction at block #" << block_index << ", tx_hash=" << tx_hash
          << " because of invalid supernode public identifier '" << stake_tx.supernode_public_id << "'");
        continue;
      }

      std::string supernode_public_address_str = cryptonote::get_account_address_as_str(m_blockchain.testnet(), stake_tx.supernode_public_address);
      std::string data = supernode_public_address_str + ":" + stake_tx.supernode_public_id;
      crypto::hash hash;
      crypto::cn_fast_hash(data.data(), data.size(), hash);

      if (!crypto::check_signature(hash, W, stake_tx.supernode_signature))
      {
        MCLOG(el::Level::Warning, "global", "Ignore stake transaction at block #" << block_index << ", tx_hash=" << tx_hash << ", supernode_public_id '" << stake_tx.supernode_public_id << "'"
          << " because of invalid supernode signature (mismatch)");
        continue;
      }

      uint64_t unlock_time = tx.unlock_time - block_index;

      if (unlock_time < config::graft::STAKE_MIN_UNLOCK_TIME)
      {
        MCLOG(el::Level::Warning, "global", "Ignore stake transaction at block #" << block_index << ", tx_hash=" << tx_hash << ", supernode_public_id '" << stake_tx.supernode_public_id << "'"
          << " because unlock time " << unlock_time << " is less than minimum allowed " << config::graft::STAKE_MIN_UNLOCK_TIME);
        continue;
      }

      if (unlock_time > config::graft::STAKE_MAX_UNLOCK_TIME)
      {
        MCLOG(el::Level::Warning, "global", "Ignore stake transaction at block #" << block_index << ", tx_hash=" << tx_hash << ", supernode_public_id '" << stake_tx.supernode_public_id << "'"
          << " because unlock time " << unlock_time << " is greater than maximum allowed " << config::graft::STAKE_MAX_UNLOCK_TIME);
        continue;
      }

      uint64_t amount = get_transaction_amount(tx, stake_tx.supernode_public_address, stake_tx.tx_secret_key);

      if (!amount)
      {
        MCLOG(el::Level::Warning, "global", "Ignore stake transaction at block #" << block_index << ", tx_hash=" << tx_hash << ", supernode_public_id '" << stake_tx.supernode_public_id << "'"
          << " because of error at parsing amount");
        continue;
      }

      stake_tx.amount = amount;
      stake_tx.block_height = block_index;
      stake_tx.hash = tx_hash;
      stake_tx.unlock_time = unlock_time;

      m_storage->add_tx(stake_tx);

      MCLOG(el::Level::Info, "global", "New stake transaction found at block #" << block_index << ", tx_hash=" << tx_hash << ", supernode_public_id '" << stake_tx.supernode_public_id
        << "', amount=" << amount / double(COIN));
    }
    catch (std::exception& e)
    {
      MCLOG(el::Level::Warning, "global", "Ignore transaction at block #" << block_index << ", tx_hash=" << tx_hash << " because of error at parsing: " << e.what());
    }
    catch (...)
    {
      MCLOG(el::Level::Warning, "global", "Ignore transaction at block #" << block_index << ", tx_hash=" << tx_hash << " because of unknown error at parsing");
    }
  }

    //update supernode stakes

  m_storage->update_supernode_stakes(block_index);

  m_stakes_need_update = true; //TODO: cache for stakes

    //update cache entries and save storage

  m_storage->add_last_processed_block(block_index, block_hash);

  if (update_storage)
    m_storage->store();
}

void StakeTransactionProcessor::process_block_blockchain_based_list(uint64_t block_index, const block& block, const crypto::hash& block_hash, bool update_storage)
{
  uint64_t prev_block_height = m_blockchain_based_list->block_height();

  m_blockchain_based_list->apply_block(block_index, block_hash, *m_storage);

  if (m_blockchain_based_list->need_store() || prev_block_height != m_blockchain_based_list->block_height())
  {
    m_blockchain_based_list_need_update = true;

    if (update_storage)
      m_blockchain_based_list->store();
  }
}

void StakeTransactionProcessor::process_block(uint64_t block_index, const block& block, const crypto::hash& block_hash, bool update_storage)
{
  process_block_stake_transaction(block_index, block, block_hash, update_storage);
  process_block_blockchain_based_list(block_index, block, block_hash, update_storage);
}

void StakeTransactionProcessor::synchronize()
{
  CRITICAL_REGION_LOCAL1(m_storage_lock);

  if (!m_storage || !m_blockchain_based_list)
    throw std::runtime_error("StakeTransactionProcessor storages have not been initialized");

  BlockchainDB& db = m_blockchain.get_db();

    //unroll already processed blocks for alternative chains
  
  for (;;)
  {
    size_t   stake_tx_count = m_storage->get_tx_count();
    uint64_t last_processed_block_index = m_storage->get_last_processed_block_index();

    if (!last_processed_block_index)
      break;
    
    const crypto::hash& last_processed_block_hash  = m_storage->get_last_processed_block_hash();
    crypto::hash        last_blockchain_block_hash = db.get_block_hash_from_height(last_processed_block_index);

    if (!memcmp(&last_processed_block_hash.data[0], &last_blockchain_block_hash.data[0], sizeof(last_blockchain_block_hash.data)))
      break; //latest block hash is the same as processed

    MCLOG(el::Level::Info, "global", "Stake transactions processing: unroll block " << last_processed_block_index);

    m_storage->remove_last_processed_block();

    if (stake_tx_count != m_storage->get_tx_count())
      m_storage->clear_supernode_stakes();

    if (m_blockchain_based_list->block_height() == last_processed_block_index)
      m_blockchain_based_list->remove_latest_block();
  }

    //apply new blocks

  uint64_t first_block_index = m_storage->get_last_processed_block_index() + 1,
           height = db.height();

  if (first_block_index > m_blockchain_based_list->block_height() + 1)
    first_block_index = m_blockchain_based_list->block_height() + 1;

  static const uint64_t SYNC_DEBUG_LOG_STEP = 10000;

  bool need_finalize_log_messages = false;
  
  for (uint64_t i=first_block_index, sync_debug_log_next_index=i + 1; i<height; i++)
  {
    if (i == sync_debug_log_next_index)
    {
      MCLOG(el::Level::Info, "global", "RTA block sync " << i << "/" << height);

      need_finalize_log_messages = true;
      sync_debug_log_next_index  = i + SYNC_DEBUG_LOG_STEP;

      if (sync_debug_log_next_index >= height)
        sync_debug_log_next_index = height - 1;
    }

    crypto::hash block_hash = db.get_block_hash_from_height(i);
    const block& block      = db.get_block(block_hash);

    process_block(i, block, block_hash, false);
  }

  if (m_blockchain_based_list->need_store())
    m_blockchain_based_list->store();

  if (m_storage->need_store())
    m_storage->store();

  if (m_stakes_need_update && m_on_stakes_update)
    invoke_update_stakes_handler_impl(height - 1);

  if (m_blockchain_based_list_need_update && m_on_blockchain_based_list_update)
    invoke_update_blockchain_based_list_handler_impl(height - first_block_index);

  if (need_finalize_log_messages)
    MCLOG(el::Level::Info, "global", "Stake transactions sync OK");
}

void StakeTransactionProcessor::set_on_update_stakes_handler(const supernode_stakes_update_handler& handler)
{
  CRITICAL_REGION_LOCAL1(m_storage_lock);
  m_on_stakes_update = handler;
}

void StakeTransactionProcessor::invoke_update_stakes_handler_impl(uint64_t block_index)
{
  try
  {
    m_on_stakes_update(block_index, m_storage->get_supernode_stakes(block_index));

    m_stakes_need_update = false;
  }
  catch (std::exception& e)
  {
    MCLOG(el::Level::Error, "global", "exception in StakeTransactionProcessor stake transactions update handler: " << e.what());
  }
}

void StakeTransactionProcessor::invoke_update_stakes_handler(bool force)
{
  CRITICAL_REGION_LOCAL1(m_storage_lock);

  if (!m_on_stakes_update)
    return;
  
  if (!m_stakes_need_update && !force)
    return;

  invoke_update_stakes_handler_impl(m_blockchain.get_db().height() - 1);
}

void StakeTransactionProcessor::set_on_update_blockchain_based_list_handler(const blockchain_based_list_update_handler& handler)
{
  CRITICAL_REGION_LOCAL1(m_storage_lock);
  m_on_blockchain_based_list_update = handler;
}

void StakeTransactionProcessor::invoke_update_blockchain_based_list_handler_impl(size_t depth)
{
  try
  {
    if (!m_blockchain_based_list->block_height())
      return; //empty blockchain based list

    if (depth > m_blockchain_based_list->history_depth())
      depth = m_blockchain_based_list->history_depth();

    if (depth > config::graft::SUPERNODE_HISTORY_SIZE)
      depth = config::graft::SUPERNODE_HISTORY_SIZE;

    uint64_t height = m_blockchain_based_list->block_height();

    for (size_t i=0; i<depth; i++)
      m_on_blockchain_based_list_update(height - i, m_blockchain_based_list->tiers(i));

    m_blockchain_based_list_need_update = false;
  }
  catch (std::exception& e)
  {
    MCLOG(el::Level::Error, "global", "exception in StakeTransactionProcessor blockchain based list update handler: " << e.what());
  }
}

void StakeTransactionProcessor::invoke_update_blockchain_based_list_handler(bool force, size_t depth)
{
  CRITICAL_REGION_LOCAL1(m_storage_lock);

  if (!m_on_blockchain_based_list_update)
    return;

  if (depth > 1)
    force = true;

  if (!m_blockchain_based_list_need_update && !force)
    return;

  invoke_update_blockchain_based_list_handler_impl(depth);
}
