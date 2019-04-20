// Copyright (c) 2017, The Graft Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "cryptonote_basic/cryptonote_basic.h"
#include "mnemonics/electrum-words.h"
#include "common/command_line.h"
#include "baseclientproxy.h"
#include "graft_defines.h"
#include "string_coding.h"
#include "common/util.h"

static const std::string scWalletCachePath("/cache/");

supernode::BaseClientProxy::BaseClientProxy()
{
}

void supernode::BaseClientProxy::Init()
{
    m_DAPIServer->ADD_DAPI_HANDLER(GetWalletBalance, rpc_command::GET_WALLET_BALANCE, BaseClientProxy);
    m_DAPIServer->ADD_DAPI_HANDLER(CreateAccount, rpc_command::CREATE_ACCOUNT, BaseClientProxy);
    m_DAPIServer->ADD_DAPI_HANDLER(GetSeed, rpc_command::GET_SEED, BaseClientProxy);
    m_DAPIServer->ADD_DAPI_HANDLER(RestoreAccount, rpc_command::RESTORE_ACCOUNT, BaseClientProxy);
    m_DAPIServer->ADD_DAPI_HANDLER(Transfer, rpc_command::TRANSFER, BaseClientProxy);
    m_DAPIServer->ADD_DAPI_HANDLER(GetTransferFee, rpc_command::GET_TRANSFER_FEE, BaseClientProxy);
}

bool supernode::BaseClientProxy::GetWalletBalance(const supernode::rpc_command::GET_WALLET_BALANCE::request &in, supernode::rpc_command::GET_WALLET_BALANCE::response &out)
{
	LOG_PRINT_L0("BaseClientProxy::GetWalletBalance" << in.Account);
    std::unique_ptr<tools::GraftWallet2> wal = initWallet(base64_decode(in.Account), in.Password);
    if (!wal)
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    try
    {
        wal->refresh();
        out.Balance = wal->balance();
        out.UnlockedBalance = wal->unlocked_balance();
        storeWalletState(wal.get());
    }
    catch (const std::exception& e)
    {
        out.Result = ERROR_BALANCE_NOT_AVAILABLE;
        return false;
    }
    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::CreateAccount(const supernode::rpc_command::CREATE_ACCOUNT::request &in, supernode::rpc_command::CREATE_ACCOUNT::response &out)
{
	LOG_PRINT_L0("BaseClientProxy::CreateAccount" << in.Language);
    std::vector<std::string> languages;
    crypto::ElectrumWords::get_language_list(languages);
    std::vector<std::string>::iterator it;
    it = std::find(languages.begin(), languages.end(), in.Language);
    if (it == languages.end())
    {
        out.Result = ERROR_LANGUAGE_IS_NOT_FOUND;
        return false;
    }

    std::unique_ptr<tools::GraftWallet2> wal =
            tools::GraftWallet2::createWallet(std::string(), m_Servant->GetNodeIp(), m_Servant->GetNodePort(),
                                             m_Servant->GetNodeLogin(), m_Servant->IsTestnet());
    if (!wal)
    {
        out.Result = ERROR_CREATE_WALLET_FAILED;
        return false;
    }
    wal->set_seed_language(in.Language);
    wal->set_refresh_from_block_height(m_Servant->GetCurrentBlockHeight());
    crypto::secret_key dummy_key;
    try
    {
        wal->generate_graft(in.Password, dummy_key, false, false);
    }
    catch (const std::exception& e)
    {
        out.Result = ERROR_CREATE_WALLET_FAILED;
        return false;
    }
    out.Account = base64_encode(wal->store_keys_graft(in.Password));
    out.Address = wal->get_account().get_public_address_str(wal->testnet());
    out.ViewKey = epee::string_tools::pod_to_hex(wal->get_account().get_keys().m_view_secret_key);
    std::string seed;
    wal->get_seed(seed);
    out.Seed = seed;
    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::GetSeed(const supernode::rpc_command::GET_SEED::request &in, supernode::rpc_command::GET_SEED::response &out)
{
	LOG_PRINT_L0("BaseClientProxy::GetSeed" << in.Account);
    std::unique_ptr<tools::GraftWallet2> wal = initWallet(base64_decode(in.Account), in.Password);
    if (!wal)
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    wal->set_seed_language(in.Language);
    std::string seed;
    wal->get_seed(seed);
    out.Seed = seed;
    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::RestoreAccount(const supernode::rpc_command::RESTORE_ACCOUNT::request &in, supernode::rpc_command::RESTORE_ACCOUNT::response &out)
{
	LOG_PRINT_L0("BaseClientProxy::RestoreAccount" << in.Seed);
    if (in.Seed.empty())
    {
        out.Result = ERROR_ELECTRUM_SEED_EMPTY;
        return false;
    }
    crypto::secret_key recovery_key;
    std::string old_language;
    if (!crypto::ElectrumWords::words_to_bytes(in.Seed, recovery_key, old_language))
    {
        out.Result = ERROR_ELECTRUM_SEED_INVALID;
        return false;
    }
    std::unique_ptr<tools::GraftWallet2> wal =
        tools::GraftWallet2::createWallet(std::string(), m_Servant->GetNodeIp(), m_Servant->GetNodePort(),
                                             m_Servant->GetNodeLogin(), m_Servant->IsTestnet());
    if (!wal)
    {
        out.Result = ERROR_CREATE_WALLET_FAILED;
        return false;
    }
    try
    {
        wal->set_seed_language(old_language);
        wal->generate_graft(in.Password, recovery_key, true, false);
        out.Account = base64_encode(wal->store_keys_graft(in.Password));
        out.Address = wal->get_account().get_public_address_str(wal->testnet());
        out.ViewKey = epee::string_tools::pod_to_hex(
                    wal->get_account().get_keys().m_view_secret_key);
        std::string seed;
        wal->get_seed(seed);
        out.Seed = seed;
    }
    catch (const std::exception &e)
    {
        out.Result = ERROR_RESTORE_WALLET_FAILED;
        return false;
    }
    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::GetTransferFee(const supernode::rpc_command::GET_TRANSFER_FEE::request &in, supernode::rpc_command::GET_TRANSFER_FEE::response &out)
{
    std::unique_ptr<tools::GraftWallet2> wal = initWallet(base64_decode(in.Account), in.Password);
    if (!wal)
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    if (wal->restricted())
    {
        //      er.code = WALLET_RPC_ERROR_CODE_DENIED;
        //      er.message = "Command unavailable in restricted mode.";
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    std::vector<cryptonote::tx_destination_entry> dsts;
    std::vector<uint8_t> extra;

    std::string payment_id = in.PaymentID;

    uint64_t amount;
    std::istringstream iss(in.Amount);
    iss >> amount;

    // validate the transfer requested and populate dsts & extra
    if (!validate_transfer(wal.get(), in.Address, amount, payment_id, dsts, extra))
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    try
    {
        uint64_t mixin = wal->default_mixin() > 0 ? wal->default_mixin() : 4;
        uint64_t unlock_time = 0;
        uint64_t priority = 0;
        std::vector<tools::GraftWallet2::pending_tx> ptx_vector =
                wal->create_transactions_2(dsts, mixin, unlock_time, priority, extra, false);

        uint64_t fees = 0;
        for (const auto ptx : ptx_vector)
        {
            fees += ptx.fee;
        }
        if (fees == 0)
        {
            out.Result = ERROR_OPEN_WALLET_FAILED;
            return false;
        }
        out.Fee = fees;
    }
    catch (const tools::error::daemon_busy& e)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_DAEMON_IS_BUSY;
        //      er.message = e.what();
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    catch (const std::exception& e)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
        //      er.message = e.what();
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    catch (...)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
        //      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::Transfer(const supernode::rpc_command::TRANSFER::request &in, supernode::rpc_command::TRANSFER::response &out)
{
    std::unique_ptr<tools::GraftWallet2> wal = initWallet(base64_decode(in.Account), in.Password);
    if (!wal)
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    if (wal->restricted())
    {
        //      er.code = WALLET_RPC_ERROR_CODE_DENIED;
        //      er.message = "Command unavailable in restricted mode.";
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    std::vector<cryptonote::tx_destination_entry> dsts;
    std::vector<uint8_t> extra;

    std::string payment_id = in.PaymentID;

    uint64_t amount;
    std::istringstream iss(in.Amount);
    iss >> amount;

    // validate the transfer requested and populate dsts & extra
    if (!validate_transfer(wal.get(), in.Address, amount, payment_id, dsts, extra))
    {
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    try
    {
        uint64_t mixin = wal->default_mixin() > 0 ? wal->default_mixin() : 4;
        uint64_t unlock_time = 0;
        uint64_t priority = 0;
        bool do_not_relay = false;
        std::vector<tools::GraftWallet2::pending_tx> ptx_vector =
                wal->create_transactions_2(dsts, mixin, unlock_time, priority, extra, false);

        if (!do_not_relay && ptx_vector.size() > 0)
        {
            wal->commit_tx(ptx_vector);
            storeWalletState(wal.get());
        }
    }
    catch (const tools::error::daemon_busy& e)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_DAEMON_IS_BUSY;
        //      er.message = e.what();
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    catch (const std::exception& e)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
        //      er.message = e.what();
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }
    catch (...)
    {
        //      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
        //      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
        out.Result = ERROR_OPEN_WALLET_FAILED;
        return false;
    }

    out.Result = STATUS_OK;
    return true;
}

bool supernode::BaseClientProxy::validate_transfer(tools::GraftWallet2 *wallet,
                                                   const string &address, uint64_t amount,
                                                   const string payment_id,
                                                   std::vector<cryptonote::tx_destination_entry> &dsts,
                                                   std::vector<uint8_t> &extra)
{
    if (!wallet)
    {
        return false;
    }
    cryptonote::tx_destination_entry de;
    bool has_payment_id;
    crypto::hash8 payment_id_short;
    //Based on PendingTransaction *WalletImpl::createTransaction()
    if(!cryptonote::get_account_integrated_address_from_str(de.addr, has_payment_id, payment_id_short, wallet->testnet(), address))
    {
        // TODO: copy-paste 'if treating as an address fails, try as url' from simplewallet.cpp:1982
//        m_status = Status_Error;
//        m_errorString = "Invalid destination address";
        return false;
    }
    de.amount = amount;
    dsts.push_back(de);

    // if dst_addr is not an integrated address, parse payment_id
    if (!has_payment_id && !payment_id.empty())
    {
        // copy-pasted from simplewallet.cpp:2212
        crypto::hash payment_id_long;
        bool r = tools::GraftWallet2::parse_long_payment_id(payment_id, payment_id_long);
        if (r)
        {
            std::string extra_nonce;
            cryptonote::set_payment_id_to_tx_extra_nonce(extra_nonce, payment_id_long);
            r = cryptonote::add_extra_nonce_to_tx_extra(extra, extra_nonce);
        }
        else
        {
            r = tools::GraftWallet2::parse_short_payment_id(payment_id, payment_id_short);
            if (r)
            {
                std::string extra_nonce;
                cryptonote::set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id_short);
                r = cryptonote::add_extra_nonce_to_tx_extra(extra, extra_nonce);
            }
        }
        if (!r)
        {
//            m_status = Status_Error;
//            m_errorString = tr("payment id has invalid format, expected 16 or 64 character hex string: ") + payment_id;
            return false;
        }
    }
    else if (has_payment_id)
    {
        std::string extra_nonce;
        cryptonote::set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id_short);
        bool r = cryptonote::add_extra_nonce_to_tx_extra(extra, extra_nonce);
        if (!r)
        {
//            m_status = Status_Error;
//            m_errorString = tr("Failed to add short payment id: ") + epee::string_tools::pod_to_hex(payment_id_short);
            return false;
        }
    }
    return true;
}

std::unique_ptr<tools::GraftWallet2> supernode::BaseClientProxy::initWallet(const string &account, const string &password) const
{
    std::unique_ptr<tools::GraftWallet2> wal;
    try
    {
        wal = tools::GraftWallet2::createWallet(account, password, "",
                                               m_Servant->GetNodeIp(), m_Servant->GetNodePort(),
                                         m_Servant->GetNodeLogin(), m_Servant->IsTestnet());
        std::string lDataDir = tools::get_default_data_dir() + scWalletCachePath;
        if (!boost::filesystem::exists(lDataDir))
        {
            boost::filesystem::create_directories(lDataDir);
        }
        std::string lCacheFile = lDataDir +
                wal->get_account().get_public_address_str(wal->testnet());
        if (boost::filesystem::exists(lCacheFile))
        {
            wal->load_cache(lCacheFile);
        }
    }
    catch (const std::exception& e)
    {
        wal = nullptr;
    }
    return wal;
}

void supernode::BaseClientProxy::storeWalletState(tools::GraftWallet2 *wallet)
{
    if (wallet)
    {
        std::string lDataDir = tools::get_default_data_dir() + scWalletCachePath;
        if (!boost::filesystem::exists(lDataDir))
        {
            boost::filesystem::create_directories(lDataDir);
        }
        std::string lCacheFile = lDataDir +
                wallet->get_account().get_public_address_str(wallet->testnet());
        wallet->store_cache(lCacheFile);
    }
}

string supernode::BaseClientProxy::base64_decode(const string &encoded_data)
{
    return epee::string_encoding::base64_decode(encoded_data);
}

string supernode::BaseClientProxy::base64_encode(const string &data)
{
    return epee::string_encoding::base64_encode(data);
}
