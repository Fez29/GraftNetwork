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
//
// Parts of this file are originally copyright (c) 2014-2017 The Monero Project

#include "gtest/gtest.h"

#include "wallet/wallet2_api.h"
//#include "wallet/wallet2.h"

#include "include_base_utils.h"
#include "cryptonote_config.h"

#include <boost/chrono/chrono.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include <iostream>
#include <vector>
#include <atomic>
#include <functional>
#include <string>
#include <chrono>
#include <thread>

#include "supernode/FSN_Servant.h"
#include "supernode/DAPI_RPC_Server.h"
#include "supernode/DAPI_RPC_Client.h"
#include "supernode/PosProxy.h"
#include "supernode/WalletProxy.h"
#include "supernode/AuthSample.h"
#include "supernode/P2P_Broadcast.h"
#include "supernode/FSN_Servant_Test.h"


using namespace supernode;
using namespace std;

namespace consts {

}


struct TestDAPI_Server_And_ClientBase : public testing::Test {
	struct TEST_RPC_CALL {
		struct request {
			BEGIN_KV_SERIALIZE_MAP()
				KV_SERIALIZE(Data)
				KV_SERIALIZE(PaymentID)
			END_KV_SERIALIZE_MAP()

			int Data;
			string PaymentID;
		};
		struct response {
			BEGIN_KV_SERIALIZE_MAP()
				KV_SERIALIZE(Data)
				KV_SERIALIZE(PaymentID)
			END_KV_SERIALIZE_MAP()

			int Data;
			string PaymentID;
		};
	};

	bool MyTestCall(const TEST_RPC_CALL::request& req, TEST_RPC_CALL::response& out) {
		out.Data = req.Data*2;
		return true;
	}

	bool Pay1(const TEST_RPC_CALL::request& req, TEST_RPC_CALL::response& out) {
		if(req.PaymentID!="1") return false;
		out.Data = 1;
		return true;
	}

	bool Pay2(const TEST_RPC_CALL::request& req, TEST_RPC_CALL::response& out) {
		if(req.PaymentID!="2") return false;
		out.Data = 2;
		return true;
	}

};

TEST_F(TestDAPI_Server_And_ClientBase, TestDAPI_Server_And_Client) {
		string ip = "127.0.0.1";
		string port = "7555";


		supernode::DAPI_RPC_Server dapi_server;
		dapi_server.Set( ip, port, 5 );

		boost::thread workerThread(&supernode::DAPI_RPC_Server::Start, &dapi_server);
		dapi_server.ADD_DAPI_HANDLER(MyTestCall, TestDAPI_Server_And_ClientBase::TEST_RPC_CALL, TestDAPI_Server_And_ClientBase);

		dapi_server.Add_UUID_MethodHandler<TEST_RPC_CALL::request, TEST_RPC_CALL::response>( "1", "Payment", bind( &TestDAPI_Server_And_ClientBase::Pay1, this, _1, _2) );
		dapi_server.Add_UUID_MethodHandler<TEST_RPC_CALL::request, TEST_RPC_CALL::response>( "2", "Payment", bind( &TestDAPI_Server_And_ClientBase::Pay2, this, _1, _2) );



		sleep(1);

		supernode::DAPI_RPC_Client client;
		client.Set(ip, port);

		TEST_RPC_CALL::request in;
		TEST_RPC_CALL::response out;
		in.Data = 10;
		out.Data = 0;
		bool ret = client.Invoke("MyTestCall", in, out);

		ASSERT_TRUE(ret && out.Data==20);

//		LOG_PRINT_L5("ret: "<<ret<<"  out.D: "<<out.Data);

		in.PaymentID = "1";
		out.Data = 0;
		ret = client.Invoke("Payment", in, out);

		ASSERT_TRUE(ret && out.Data==1);
//		LOG_PRINT_L5("ret: "<<ret<<"  out.D: "<<out.Data);

		in.PaymentID = "2";
		ret = client.Invoke("Payment", in, out);

		ASSERT_TRUE(ret && out.Data==2);
//		LOG_PRINT_L5("ret: "<<ret<<"  out.D: "<<out.Data);


		dapi_server.Stop();

		workerThread.join();

};


namespace supernode {




struct Test_RTA_FlowBlockChain : public testing::Test {
	string s_TestDataPath;// = "/home/laid/Dev/Graft/GraftNetwork/build/debug/tests/data/supernode";

	public:
	struct Supernode {
		FSN_Servant* Servant = nullptr;
		boost::thread* WorkerThread = nullptr;
		supernode::DAPI_RPC_Server dapi_server;
		string s_TestDataPath;

		string Port;
		string IP;
		atomic_uint Started = {0};


		Supernode(const string& path) { s_TestDataPath = path; }

		void Run() {
			dapi_server.Set( IP, Port, 500 );


			vector<supernode::BaseRTAProcessor*> objs;
			objs.push_back( new supernode::WalletProxy() );
			objs.push_back( new supernode::PosProxy() );
			objs.push_back( new supernode::AuthSample() );

			for(unsigned i=0;i<objs.size();i++) {
				objs[i]->Set(Servant, &dapi_server);
				objs[i]->Start();
			}

			LOG_PRINT_L5("DAPI START on: "<<Port);
			Started = 1;
			dapi_server.Start();// block execution


			// -----------------------------------------------------------------

			for(unsigned i=0;i<objs.size();i++) {
				objs[i]->Stop();
				delete objs[i];
			}

		}


		void Start(string p1, string p2, bool second) {
			IP = "127.0.0.1";
			Port = second?p2:p1;


			string db_path = s_TestDataPath + "/test_blockchain";
			string wallet_root_path = s_TestDataPath + "/test_wallets";

			string wss1 = "/stake_wallet";
			string wss2 = "/miner_wallet";
			if(second) swap(wss1, wss2);


			Servant = new FSN_Servant_Test(db_path, "localhost:28281", "", true);
			Servant->Set(wallet_root_path + wss1, "", wallet_root_path + wss2, "");

		    // wallet1
		    string address1 = "T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE";
		    string viewkey1 = "0ae7176e5332974de64713c329d406956e8ff2fd60c85e7ee6d8c88318111007";
		    // wallet2
		    string address2 = "T6SnKmirXp6geLAoB7fn2eV51Ctr1WH1xWDnEGzS9pvQARTJQUXupiRKGR7czL7b5XdDnYXosVJu6Wj3Y3NYfiEA2sU2QiGVa";
		    string viewkey2 = "8c0ccff03e9f2a9805e200f887731129495ff793dc678db6c5b53df814084f04";


			Servant->AddFsnAccount(boost::make_shared<FSN_Data>(FSN_WalletData{address1, viewkey1}, FSN_WalletData{address2, viewkey2}, "127.0.0.1", p1));
			Servant->AddFsnAccount(boost::make_shared<FSN_Data>(FSN_WalletData{address2, viewkey2}, FSN_WalletData{address1, viewkey1}, "127.0.0.1", p2));


			LOG_PRINT_L5("STARTED: "<<(second?2:1));


			WorkerThread = new boost::thread(&Supernode::Run, this);
		}

		void Stop() {
			dapi_server.Stop();
			WorkerThread->join();
		}



	};



	NTransactionStatus GetPayStatus(const string& payID) {
		DAPI_RPC_Client call;
		call.Set(IP, WalletProxyPort);

		rpc_command::WALLET_GET_TRANSACTION_STATUS::request in;
		rpc_command::WALLET_GET_TRANSACTION_STATUS::response out;
		in.PaymentID = payID;
		bool ret = call.Invoke(dapi_call::GetPayStatus, in, out, chrono::seconds(10));

        if(!ret) return NTransactionStatus::Fail;
        return NTransactionStatus(out.Status);
	}

	NTransactionStatus GetSaleStatus(const string& payID) {
		DAPI_RPC_Client call;
		call.Set(IP, PosProxyPort);

		rpc_command::POS_GET_SALE_STATUS::request in;
		rpc_command::POS_GET_SALE_STATUS::response out;
		in.PaymentID = payID;
		bool ret = call.Invoke(dapi_call::GetSaleStatus, in, out, chrono::seconds(10));

        if(!ret) return NTransactionStatus::Fail;
        return NTransactionStatus(out.Status);
	}

	string IP = "127.0.0.1";;
	string WalletProxyPort = "7500";;
	string PosProxyPort = "8500";;
	bool Verbose = true;

	bool Assert(bool bb, const string& str) {
		if(!Verbose) return bb;
		LOG_PRINT_L5(str<<" - "<<(bb?"OK":"Fail"));
		return bb;
	}

	bool DoTest() {
		bool bb;

		rpc_command::POS_SALE::request sale_in;
		rpc_command::POS_SALE::response sale_out;
        sale_in.Amount = 11;
        sale_in.POSSaleDetails = "Some data";
        sale_in.POSAddress = "0xFF";

		unsigned repeatCount = 10;


		for(unsigned i=0;i<repeatCount;i++) {// transaction must started from Sale call
			DAPI_RPC_Client pos_sale;
			pos_sale.Set(IP, PosProxyPort);
			bb = pos_sale.Invoke("Sale", sale_in, sale_out, chrono::seconds(10));
			if( Assert(bb, "Sale") ) break;

			//LOG_PRINT_L5("Sale ret: "<<ret<<"  BlockNum: "<<sale_out.BlockNum<<"  uuid: "<<sale_out.PaymentID);
		}
		if(!bb) return false;

		for(unsigned i=0;i<repeatCount;i++) {// after sale call you get PaymentID and BlockNum and can start poll status by GetSaleStatus call
			NTransactionStatus trs =  GetSaleStatus(sale_out.PaymentID);
			bb = trs==NTransactionStatus::InProgress;
			if( Assert(bb, "GetSaleStatus") ) break;
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
			//LOG_PRINT_L5("GetSaleStatus: "<<()<<"  int: "<<int(trs));
		}
		if(!bb) return false;

		for(unsigned i=0;i<repeatCount;i++) {// in any time after Sale call you can get PoS data by WalletGetPosData call
			rpc_command::WALLET_GET_POS_DATA::request in;
			rpc_command::WALLET_GET_POS_DATA::response out;
			in.BlockNum = sale_out.BlockNum;
			in.PaymentID = sale_out.PaymentID;
			DAPI_RPC_Client call;
			call.Set(IP, WalletProxyPort);
			bb = call.Invoke("WalletGetPosData", in, out, chrono::seconds(10));
            bb = bb && out.POSSaleDetails==sale_in.POSSaleDetails;
			if( Assert(bb, "WalletGetPosData") ) break;
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(100));

			//LOG_PRINT_L5("WalletGetPosData ret: "<<ret<<"  data: "<<out.DataForClientWallet);
		}
		if(!bb) return false;


		// after use push Pay button, send Pay call
		rpc_command::WALLET_PAY::request pay_in;
		rpc_command::WALLET_PAY::response pay_out;
        pay_in.Amount = sale_in.Amount;
        pay_in.POSAddress = sale_in.POSAddress;
		pay_in.BlockNum = sale_out.BlockNum;
		pay_in.PaymentID = sale_out.PaymentID;
		for(unsigned i=0;i<repeatCount;i++) {
			DAPI_RPC_Client wallet_pay;
			wallet_pay.Set(IP, WalletProxyPort);
			bb = wallet_pay.Invoke("Pay", pay_in, pay_out, chrono::seconds(10));
			if( Assert(bb, "Pay") ) break;
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
			//LOG_PRINT_L5("Pay ret: "<<ret);
		}
		if(!bb) return false;

		for(unsigned i=0;i<repeatCount;i++) {// after Pay call you can can start poll status by GetPayStatus call
			NTransactionStatus trs =  GetPayStatus(sale_out.PaymentID);
			bb = trs==NTransactionStatus::Success;
			if( Assert(bb, "GetPayStatus") ) break;
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
			//LOG_PRINT_L5("GetPayStatus: "<<(trs==NTRansactionStatus::Success)<<"  int: "<<int(trs));

		}
		if(!bb) return false;


		for(unsigned i=0;i<repeatCount;i++) {
			NTransactionStatus trs =  GetSaleStatus(sale_out.PaymentID);
			bb = trs==NTransactionStatus::Success;
			if( Assert(bb, "GetSaleStatus") ) break;
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
			//LOG_PRINT_L5("GetSaleStatus2: "<<(trs==NTRansactionStatus::Success)<<"  int: "<<int(trs));
		}
		if(!bb) return false;


		return true;
	}

	bool TestWalletReject() {
		bool bb;

		rpc_command::POS_SALE::request sale_in;
		rpc_command::POS_SALE::response sale_out;
        sale_in.Amount = 11;
        sale_in.POSSaleDetails = "Some data";
        sale_in.POSAddress = "0xFF";

		unsigned repeatCount = 10;


		for(unsigned i=0;i<repeatCount;i++) {// transaction must started from Sale call
			DAPI_RPC_Client pos_sale;
			pos_sale.Set(IP, PosProxyPort);
			bb = pos_sale.Invoke("Sale", sale_in, sale_out, chrono::seconds(10));
			if( Assert(bb, "Sale") ) break;
		}
		if(!bb) return false;

		for(unsigned i=0;i<repeatCount;i++) {// after sale call you get PaymentID and BlockNum and can start poll status by GetSaleStatus call
			NTransactionStatus trs =  GetSaleStatus(sale_out.PaymentID);
			bb = trs==NTransactionStatus::InProgress;
			if( Assert(bb, "GetSaleStatus") ) break;
		}
		if(!bb) return false;

		for(unsigned i=0;i<repeatCount;i++) {// in any time after Sale call you can get PoS data by WalletGetPosData call
			rpc_command::WALLET_REJECT_PAY::request in;
			rpc_command::WALLET_REJECT_PAY::response out;
			in.BlockNum = sale_out.BlockNum;
			in.PaymentID = sale_out.PaymentID;
			DAPI_RPC_Client call;
			call.Set(IP, WalletProxyPort);
			bb = call.Invoke("WalletRejectPay", in, out, chrono::seconds(10));
			if( Assert(bb, "WalletRejectPay") ) break;
		}
		if(!bb) return false;



		for(unsigned i=0;i<repeatCount;i++) {// after Pay call you can can start poll status by GetPayStatus call
			NTransactionStatus trs =  GetSaleStatus(sale_out.PaymentID);
			bb = trs==NTransactionStatus::RejectedByWallet;
			if( Assert(bb, "GetSaleStatus : RejectedByWallet") ) break;
		}
		if(!bb) return false;

		LOG_PRINT_L5("WalletRejectPay - OK ");
		return true;

	}

	unsigned m_RunInTread = 10;
	atomic_uint m_Fail = {0};

	void TestThread() {
		for(unsigned i=0;i<m_RunInTread;i++) {
			LOG_PRINT_L5("\n");
			if( !DoTest() ) { m_Fail++; }
			//boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));
		}
	}
};

};

TEST_F(Test_RTA_FlowBlockChain, Test_RTA_With_FlowBlockChain) {
	s_TestDataPath = epee::string_tools::get_current_module_folder() + "/../data/supernode";

	Supernode wallet_proxy(s_TestDataPath);
	wallet_proxy.Start(WalletProxyPort, PosProxyPort, false);

	Supernode pos_proxy(s_TestDataPath);
	pos_proxy.Start(WalletProxyPort, PosProxyPort, true);

	while(!wallet_proxy.Started || !pos_proxy.Started) boost::this_thread::sleep( boost::posix_time::milliseconds(100) );
	sleep(1);


//	TestWalletReject();

	//m_RunInTread=1;
	//TestThread();

	ASSERT_TRUE( TestWalletReject() );

	boost::thread_group workers;
	for(int i=0;i<10;i++) {
		workers.create_thread( boost::bind(&Test_RTA_FlowBlockChain::TestThread, this) );
	}
	workers.join_all();

	ASSERT_TRUE( m_Fail==0 );

//		LOG_PRINT_L5("\n\nFAILED count: "<<m_Fail);


	wallet_proxy.Stop();
	pos_proxy.Stop();
}



struct FSNServantTest : public testing::Test
{

    FSN_Servant * fsns = nullptr;
    string db_path = epee::string_tools::get_current_module_folder() + "/../data/supernode/test_blockchain";


    FSNServantTest()
    {
        fsns = new FSN_Servant(db_path, "localhost:28281", "", true);

    }

    ~FSNServantTest()
    {
        delete fsns;
    }
};


TEST_F(FSNServantTest, CreateDestroyInstance)
{
    ASSERT_TRUE(fsns);
}

TEST_F(FSNServantTest, ProofOfStakeTestMiner)
{
    // miner wallet1
    const string address1 = "T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE";
    const string viewkey1 = "0ae7176e5332974de64713c329d406956e8ff2fd60c85e7ee6d8c88318111007";
    // miner wallet2
    const string address2 = "T6SgjB6ps9Z5cizMGGaLvo5SbyW7eoqV4es7V73oPPPuKJVPtrtBueX1pM62zezfev7DwEUKHN8UZ8kE6fgVc4X32JWVErmSD";
    const string viewkey2 = "f5fc5db98492ee75964d81f2ec313a567fea73f57ed9b31d9085f42055798d07";

    fsns->AddFsnAccount(boost::make_shared<FSN_Data>(FSN_WalletData{"", ""}, FSN_WalletData{address1, viewkey1}));
    fsns->AddFsnAccount(boost::make_shared<FSN_Data>(FSN_WalletData{"", ""}, FSN_WalletData{address2, viewkey2}));

    vector<pair<uint64_t, boost::shared_ptr<FSN_Data>>> output =
            fsns->LastBlocksResolvedByFSN(1, 1);
    std::cout << "size: " << output.size() << std::endl;
    ASSERT_TRUE(output.size() == 1);
    ASSERT_TRUE(output[0].first == 1);
    ASSERT_TRUE(output[0].second->Miner.Addr == address1);
    output.clear();
    output = fsns->LastBlocksResolvedByFSN(1, 10);

    ASSERT_TRUE(output.size() == 10);

    ASSERT_TRUE(output[0].first == 10);
    ASSERT_TRUE(output[1].first == 9);
    ASSERT_TRUE(output[2].first == 8);
    ASSERT_TRUE(output[3].first == 7);
    ASSERT_TRUE(output[4].first == 6);
    ASSERT_TRUE(output[5].first == 5);
    ASSERT_TRUE(output[6].first == 4);
    ASSERT_TRUE(output[7].first == 3);
    ASSERT_TRUE(output[8].first == 2);
    ASSERT_TRUE(output[9].first == 1);


    ASSERT_TRUE(output[0].second->Miner.Addr == address2);
    ASSERT_TRUE(output[9].second->Miner.Addr == address1);

    output = fsns->LastBlocksResolvedByFSN(2000000, 10);
    ASSERT_TRUE(output.empty());

    output = fsns->LastBlocksResolvedByFSN(20, 10000);
    ASSERT_FALSE(output.empty());

    for (const auto &iter : output) {
        ASSERT_TRUE(iter.second->Miner.Addr == address1 || iter.second->Miner.Addr == address2);
    }
}

TEST_F(FSNServantTest, SetStakeAndMinerWallets)
{
    string wallet_root_path = epee::string_tools::get_current_module_folder() + "/../data/supernode/test_wallets";
    bool set_failed = false;
    try {
        fsns->Set(wallet_root_path + "/stake_wallet", "", wallet_root_path + "/miner_wallet", "");
    } catch (...) {
        set_failed = true;
    }
    ASSERT_FALSE(set_failed);
    ASSERT_TRUE(fsns->GetMyMinerWallet().Addr == "T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE");
    ASSERT_TRUE(fsns->GetMyMinerWallet().ViewKey == "0ae7176e5332974de64713c329d406956e8ff2fd60c85e7ee6d8c88318111007");
    ASSERT_TRUE(fsns->GetMyStakeWallet().Addr == "T6SnKmirXp6geLAoB7fn2eV51Ctr1WH1xWDnEGzS9pvQARTJQUXupiRKGR7czL7b5XdDnYXosVJu6Wj3Y3NYfiEA2sU2QiGVa");
    ASSERT_TRUE(fsns->GetMyStakeWallet().ViewKey == "8c0ccff03e9f2a9805e200f887731129495ff793dc678db6c5b53df814084f04");
}

TEST_F(FSNServantTest, SignAndVerify)
{
    string wallet_root_path = epee::string_tools::get_current_module_folder() + "/../data/supernode/test_wallets";
    fsns->Set(wallet_root_path + "/stake_wallet", "", wallet_root_path + "/miner_wallet", "");

    std::string message = "Hello, Graft";
    std::string address = "T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE";

    std::string signature = fsns->SignByWalletPrivateKey(message, address);
    std::cout << "signature: " << signature << std::endl;

    ASSERT_TRUE(fsns->IsSignValid(message, address, signature));
    ASSERT_FALSE(fsns->IsSignValid(message + ".", address, signature));
}


TEST_F(FSNServantTest, GetBalance1)
{
    FSN_WalletData wallet1("T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE",
                           "0ae7176e5332974de64713c329d406956e8ff2fd60c85e7ee6d8c88318111007");

    uint64_t balance_10block = fsns->GetWalletBalance(10, wallet1);
    ASSERT_TRUE(balance_10block > 0);
    uint64_t balance_50block = fsns->GetWalletBalance(50, wallet1);
    ASSERT_TRUE(balance_50block > 0);
    ASSERT_TRUE(balance_10block < balance_50block);


}


TEST_F(FSNServantTest, GetBalance2)
{
    FSN_WalletData wallet1("T6T2LeLmi6hf58g7MeTA8i4rdbVY8WngXBK3oWS7pjjq9qPbcze1gvV32x7GaHx8uWHQGNFBy1JCY1qBofv56Vwb26Xr998SE",
                           "0ae7176e5332974de64713c329d406956e8ff2fd60c85e7ee6d8c88318111007");

//    std::cout << Monero::Wallet::displayAmount(fsns->GetWalletBalance(20, wallet1)) << std::endl;
//    std::cout << Monero::Wallet::displayAmount(fsns->GetWalletBalance(10, wallet1)) << std::endl;
//    for (int i = 0; i < 1; ++i) {
//        std::cout << "opening wallet: " << i << std::endl;
//        std::cout << Monero::Wallet::displayAmount(fsns->GetWalletBalance(0,  wallet1)) << std::endl;
//    }

//    ASSERT_TRUE(fsns->GetWalletBalance(0, wallet1) > 0);
//    auto start = std::chrono::high_resolution_clock::now();
//    std::this_thread::sleep_for(std::chrono::seconds(2));
//    auto end = std::chrono::high_resolution_clock::now();
//    std::chrono::duration<double, std::milli> elapsed = end-start;
//    std::cout << "Waited " << elapsed.count() << " ms\n";

}


int main(int argc, char** argv)
{
    epee::string_tools::set_module_name_and_folder(argv[0]);
    mlog_configure("", true);
    mlog_set_log_level(1);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

