/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#pragma once

#include "contracts.hpp"
#include "test_symbol.hpp"

#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/testing/tester.hpp>
#include <fc/variant_object.hpp>

#include <fstream>

using namespace eosio::chain;
using namespace eosio::testing;
using namespace fc;

using mvo = fc::mutable_variant_object;

#ifndef TESTER
# ifdef NON_VALIDATING_TEST
#  define TESTER tester
# else
#  define TESTER validating_tester
# endif
#endif


// just sugar
#define STRSYM(str) core_sym::from_string(str)

/// Number of initially issued tokens.
#define TOKENS_ISSUED 167270821L

#define TOKEN_PRECISION 4
#define TOKEN_FRACTIONAL_PART_MULTIPLIER 10000 // 10^TOKEN_PRECISION


namespace eosio_system {

class eosio_system_tester : public TESTER {
public:

   enum class setup_level {
      none,
      minimal,
      core_token,
      deploy_contract,
      full
   };

   // contructors

   explicit eosio_system_tester( setup_level l = setup_level::full ) {
      if( l == setup_level::none ) return;

      BOOST_TEST_MESSAGE("basic_setup();");
      basic_setup();
      if( l == setup_level::minimal ) return;

      BOOST_TEST_MESSAGE("create_core_token();");
      create_core_token();
      if( l == setup_level::core_token ) return;

      BOOST_TEST_MESSAGE("deploy_contract();");
      deploy_contract();
      if( l == setup_level::deploy_contract ) return;

      BOOST_TEST_MESSAGE("remaining_setup();");
      remaining_setup();
   }

   template<typename Lambda>
   explicit eosio_system_tester(Lambda setup) {
      setup(*this);

      basic_setup();
      create_core_token();
      deploy_contract();
      remaining_setup();
   }

   // members

   void basic_setup() {
      produce_blocks( 2 );

      create_accounts({
         N(dao),
         N(eosio.saving),
         N(eosio.bpay),
         N(eosio.names),
         N(eosio.ram),
         N(eosio.ramfee),
         N(eosio.stake),
         N(eosio.token),
         N(eosio.vpay)
      });

      produce_blocks( 100 );
      set_code( N(eosio.token), contracts::token_wasm());
      set_abi( N(eosio.token), contracts::token_abi().data() );
      {
         const auto& accnt = control->db().get<account_object,by_name>( N(eosio.token) );
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
         token_abi_ser.set_abi(abi, abi_serializer_max_time);
      }
   }

   void create_core_token( symbol core_symbol = symbol{CORE_SYM} ) {
      const int64_t tokens_issued_asset = TOKENS_ISSUED * TOKEN_FRACTIONAL_PART_MULTIPLIER;

      FC_ASSERT( core_symbol.decimals() == TOKEN_PRECISION, "create_core_token assumes precision of core token is 4" );
      create_currency( N(eosio.token), config::system_account_name, asset(100000000000000, core_symbol) );
      issue( asset(tokens_issued_asset, core_symbol) );
      BOOST_REQUIRE_EQUAL( asset(tokens_issued_asset, core_symbol), get_balance( "eosio", core_symbol ) );
   }

   void deploy_contract( bool call_init = true ) {
      set_code( config::system_account_name, contracts::system_wasm() );
      set_abi( config::system_account_name, contracts::system_abi().data() );
      if( call_init ) {
         base_tester::push_action(config::system_account_name, N(init),
                                               config::system_account_name,  mutable_variant_object()
                                               ("version", 0)
                                               ("core",    CORE_SYM_STR));
      }

      {
         const auto& accnt = control->db().get<account_object,by_name>( config::system_account_name );
         abi_def abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
         abi_ser.set_abi(abi, abi_serializer_max_time);
      }
   }

   void remaining_setup() {
      produce_blocks();

      // Assumes previous setup steps were done with core token symbol set to CORE_SYM
      create_account_with_resources( N(alice1111111), config::system_account_name, STRSYM("1.0000"), false );
      create_account_with_resources( N(bob111111111), config::system_account_name, STRSYM("0.4500"), false );
      create_account_with_resources( N(carol1111111), config::system_account_name, STRSYM("1.0000"), false );

      debug_balances({ N(eosio), N(eosio.ramfee), N(eosio.stake), N(eosio.ram) });

      const std::string eosio_total_balances = std::to_string(TOKENS_ISSUED) + "." + std::string(TOKEN_PRECISION, '0');
      BOOST_REQUIRE_EQUAL( STRSYM(eosio_total_balances),
         get_balance("eosio") + get_balance("eosio.ramfee") + get_balance("eosio.stake") + get_balance("eosio.ram") );
   }


   void create_accounts_with_resources( const vector<account_name>& accounts, const account_name& creator = config::system_account_name ) {
      for( const auto& a : accounts ) {
         create_account_with_resources( a, creator );
      }
   }

   transaction_trace_ptr create_account_with_resources( const account_name& a, const account_name& creator, uint32_t ram_bytes = 8000 ) {
      signed_transaction trx;
      set_transaction_headers(trx);

      authority owner_auth;
      owner_auth =  authority( get_public_key( a, "owner" ) );

      trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                newaccount{
                                   .creator  = creator,
                                   .name     = a,
                                   .owner    = owner_auth,
                                   .active   = authority( get_public_key( a, "active" ) )
                                });

      trx.actions.emplace_back( get_action( config::system_account_name, N(buyrambytes), vector<permission_level>{{creator,config::active_name}},
                                            mvo()
                                            ("payer",    creator)
                                            ("receiver", a)
                                            ("bytes",    ram_bytes) ) );
      const auto a_net = STRSYM("10.0000");
      const auto a_cpu = STRSYM("10.0000");
      const auto a_vote = STRSYM("0.0000");
      trx.actions.emplace_back( get_action( config::system_account_name, N(delegatebw), vector<permission_level>{{creator,config::active_name}},
                                            mvo()
                                            ("from",                creator)
                                            ("receiver",            a)
                                            ("stake_net_quantity",  a_net)
                                            ("stake_cpu_quantity",  a_cpu)
                                            ("stake_vote_quantity", a_vote)
                                            ("transfer", false ) ) );
      BOOST_TEST_MESSAGE(creator << " creates acount " << a << " with net = " << a_net << ", cpu = " << a_cpu << ", vote = " << a_vote);

      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), control->get_chain_id() );
      return push_transaction( trx );
   }

   transaction_trace_ptr create_account_with_resources( const account_name& a,
                                                        const account_name& creator,
                                                        const asset& ramfunds,
                                                        bool multisig,
                                                        asset net = STRSYM("10.0000"),
                                                        asset cpu = STRSYM("10.0000"),
                                                        asset vote = STRSYM("0.0000"),
                                                        bool transfer = false) {
      signed_transaction trx;
      set_transaction_headers(trx);

      authority owner_auth;
      if (multisig) {
         // multisig between account's owner key and creators active permission
         owner_auth = authority(2, {key_weight{get_public_key( a, "owner" ), 1}}, {permission_level_weight{{creator, config::active_name}, 1}});
      } else {
         owner_auth =  authority( get_public_key( a, "owner" ) );
      }

      trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                newaccount{
                                   .creator  = creator,
                                   .name     = a,
                                   .owner    = owner_auth,
                                   .active   = authority( get_public_key( a, "active" ) )
                                });

      trx.actions.emplace_back( get_action( config::system_account_name, N(buyram), vector<permission_level>{{creator,config::active_name}},
                                            mvo()
                                            ("payer",    creator)
                                            ("receiver", a)
                                            ("quant",    ramfunds) ) );

      trx.actions.emplace_back( get_action( config::system_account_name, N(delegatebw), vector<permission_level>{{creator,config::active_name}},
                                            mvo()
                                            ("from",                creator)
                                            ("receiver",            a)
                                            ("stake_net_quantity",  net)
                                            ("stake_cpu_quantity",  cpu)
                                            ("stake_vote_quantity", vote)
                                            ("transfer",            transfer) ) );
      BOOST_TEST_MESSAGE(creator << " creates acount " << a <<
         " with net = " << net << ", cpu = " << cpu << ", vote = " << vote << ", ramfunds = " << ramfunds);

      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), control->get_chain_id()  );
      return push_transaction( trx );
   }

   transaction_trace_ptr setup_producer_accounts( const std::vector<account_name>& accounts,
                                                  asset ram = STRSYM("1.0000"),
                                                  asset cpu = STRSYM("80.0000"),
                                                  asset net = STRSYM("80.0000"),
                                                  asset vote = STRSYM("0.0000")
                                                )
   {
      account_name creator(config::system_account_name);
      signed_transaction trx;
      set_transaction_headers(trx);

      for (const auto& a: accounts) {
         authority owner_auth( get_public_key( a, "owner" ) );
         trx.actions.emplace_back( vector<permission_level>{{creator,config::active_name}},
                                   newaccount{
                                      .creator = creator,
                                      .name    = a,
                                      .owner   = owner_auth,
                                      .active  = authority( get_public_key( a, "active" ) )
                                   });

         trx.actions.emplace_back( get_action( config::system_account_name, N(buyram), vector<permission_level>{ {creator, config::active_name} },
                                               mvo()
                                               ("payer",    creator)
                                               ("receiver", a)
                                               ("quant",    ram) ) );

         trx.actions.emplace_back( get_action( config::system_account_name, N(delegatebw), vector<permission_level>{ {creator, config::active_name} },
                                               mvo()
                                               ("from",                creator)
                                               ("receiver",            a)
                                               ("stake_net_quantity",  net)
                                               ("stake_cpu_quantity",  cpu)
                                               ("stake_vote_quantity", vote)
                                               ("transfer",            false) ) );
      }

      set_transaction_headers(trx);
      trx.sign( get_private_key( creator, "active" ), control->get_chain_id()  );
      return push_transaction( trx );
   }

   action_result buyram( const account_name& payer, const account_name& receiver, const asset& quant ) {
      return push_action( payer, N(buyram), mvo()
                          ("payer",    payer)
                          ("receiver", receiver)
                          ("quant",    quant) );
   }
   action_result buyrambytes( const account_name& payer, const account_name& receiver, uint32_t numbytes ) {
      return push_action( payer, N(buyrambytes), mvo()
                          ("payer",    payer)
                          ("receiver", receiver)
                          ("bytes",    numbytes) );
   }

   action_result sellram( const account_name& account, uint64_t numbytes ) {
      return push_action( account, N(sellram), mvo()
                          ("account", account)
                          ("bytes",   numbytes) );
   }

   action_result push_action( const account_name& signer, const action_name &name, const variant_object& data, bool auth = true ) {
         string action_type_name = abi_ser.get_action_type(name);

         action act;
         act.account = config::system_account_name;
         act.name = name;
         act.data = abi_ser.variant_to_binary( action_type_name, data, abi_serializer_max_time );

         return base_tester::push_action( std::move(act), auth ? uint64_t(signer) : signer == N(bob111111111) ? N(alice1111111) : N(bob111111111) );
   }

   action_result stake( const account_name& from, const account_name& to, const asset& net, const asset& cpu, const asset& vote ) {
      return push_action( name(from), N(delegatebw), mvo()
                          ("from",                from)
                          ("receiver",            to)
                          ("stake_net_quantity",  net)
                          ("stake_cpu_quantity",  cpu)
                          ("stake_vote_quantity", vote)
                          ("transfer",            false) );
   }

   action_result stake( const account_name& acnt, const asset& net, const asset& cpu, const asset& vote ) {
      return stake( acnt, acnt, net, cpu, vote );
   }

   action_result stake_with_transfer( const account_name& from, const account_name& to, const asset& net, const asset& cpu, const asset& vote ) {
      return push_action( name(from), N(delegatebw), mvo()
                          ("from",                from)
                          ("receiver",            to)
                          ("stake_net_quantity",  net)
                          ("stake_cpu_quantity",  cpu)
                          ("stake_vote_quantity", vote)
                          ("transfer",            true) );
   }

   action_result unstake( const account_name& from, const account_name& to, const asset& net, const asset& cpu, const asset& vote ) {
      return push_action( name(from), N(undelegatebw), mvo()
                          ("from",     from)
                          ("receiver", to)
                          ("unstake_net_quantity", net)
                          ("unstake_cpu_quantity", cpu)
                          ("unstake_vote_quantity", vote) );
   }

   action_result unstake( const account_name& acnt, const asset& net, const asset& cpu, const asset& vote ) {
      return unstake( acnt, acnt, net, cpu, vote );
   }

   action_result bidname( const account_name& bidder, const account_name& newname, const asset& bid ) {
      return push_action( name(bidder), N(bidname), mvo()
                          ("bidder",  bidder)
                          ("newname", newname)
                          ("bid",     bid) );
   }

   static fc::variant_object producer_parameters_example( int n ) {
      return mutable_variant_object()
         ("max_block_net_usage", 10000000 + n )
         ("target_block_net_usage_pct", 10 + n )
         ("max_transaction_net_usage", 1000000 + n )
         ("base_per_transaction_net_usage", 100 + n)
         ("net_usage_leeway", 500 + n )
         ("context_free_discount_net_usage_num", 1 + n )
         ("context_free_discount_net_usage_den", 100 + n )
         ("max_block_cpu_usage", 10000000 + n )
         ("target_block_cpu_usage_pct", 10 + n )
         ("max_transaction_cpu_usage", 1000000 + n )
         ("min_transaction_cpu_usage", 100 + n )
         ("max_transaction_lifetime", 3600 + n)
         ("deferred_trx_expiration_window", 600 + n)
         ("max_transaction_delay", 10*86400+n)
         ("max_inline_action_size", 4096 + n)
         ("max_inline_action_depth", 4 + n)
         ("max_authority_depth", 6 + n)
         ("max_ram_size", (n % 10 + 1) * 1024 * 1024)
         ("ram_reserve_ratio", 100 + n);
   }

   action_result regproducer( const account_name& acnt ) {
      action_result r = push_action( acnt, N(regproducer), mvo()
                          ("producer",     acnt)
                          ("producer_key", get_public_key( acnt, "active" ))
                          ("url",          "")
                          ("location",     0) );
      BOOST_REQUIRE_EQUAL( success(), r);
      return r;
   }

   action_result vote( const account_name& voter, const std::vector<account_name>& producers, const account_name& proxy = name(0) ) {
      return push_action(voter, N(voteproducer), mvo()
                         ("voter",     voter)
                         ("proxy",     proxy)
                         ("producers", producers));
   }

   asset get_balance( const account_name& act, symbol balance_symbol = symbol{CORE_SYM} ) const {
      vector<char> data = get_row_by_account( N(eosio.token), act, N(accounts), balance_symbol.to_symbol_code().value );
      return data.empty() ? asset(0, balance_symbol) : token_abi_ser.binary_to_variant("account", data, abi_serializer_max_time)["balance"].as<asset>();
   }

   fc::variant get_total_stake( const account_name& act ) const {
      vector<char> data = get_row_by_account( config::system_account_name, act, N(userres), act );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "user_resources", data, abi_serializer_max_time );
   }

   fc::variant get_voter_info( const account_name& act ) {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(voters), act );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "voter_info", data, abi_serializer_max_time );
   }

   fc::variant get_producer_info( const account_name& act ) {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(producers), act );
      return abi_ser.binary_to_variant( "producer_info", data, abi_serializer_max_time );
   }

   fc::variant get_producer_info2( const account_name& act ) {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(producers2), act );
      return abi_ser.binary_to_variant( "producer_info2", data, abi_serializer_max_time );
   }

   fc::variant get_name_bid( const account_name& act ) const {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(namebids), act );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "name_bid", data, abi_serializer_max_time );
   }

   void debug_name_bids( const std::vector<account_name>& accounts ) const {
      for (const auto& a : accounts) {
         std::stringstream bid;
         bid << get_name_bid(a);

         BOOST_TEST_MESSAGE("name bid for " << a.to_string() << ": " << bid.str());
      }
   }

   void debug_balances( const std::vector<account_name>& accounts ) const {
      for (const auto& a : accounts) {
         //TODO: WTF "error: no member named 'to_string' in 'fc::variant'"???!!!
         std::stringstream stake;
         stake << get_total_stake(a);

         BOOST_TEST_MESSAGE(a.to_string()
            << ": balance: " << get_balance(a)
            << ", user_resources: " << stake.str());
      }
   }

   void create_currency( const name& contract, const name& manager, const asset& maxsupply ) {
      auto act = mutable_variant_object()
         ("issuer",         manager)
         ("maximum_supply", maxsupply);

      base_tester::push_action(contract, N(create), contract, act);
   }

   void issue( const asset& amount, const name& manager = config::system_account_name ) {
      base_tester::push_action( N(eosio.token), N(issue), manager, mutable_variant_object()
                                ("to",       manager)
                                ("quantity", amount)
                                ("memo",     "") );
   }
   void transfer( const name& from, const name& to, const asset& amount, const name& manager = config::system_account_name ) {
      base_tester::push_action( N(eosio.token), N(transfer), manager, mutable_variant_object()
                                       ("from",     from)
                                       ("to",       to)
                                       ("quantity", amount)
                                       ("memo",     "") );
   }

   void issue_and_transfer( const name& to, const asset& amount, const name& manager = config::system_account_name ) {
      signed_transaction trx;
      trx.actions.emplace_back( get_action( N(eosio.token), N(issue),
                                            vector<permission_level>{{manager, config::active_name}},
                                            mutable_variant_object()
                                            ("to",       manager )
                                            ("quantity", amount )
                                            ("memo",     "")
                                            )
                                );
      if ( to != manager ) {
         trx.actions.emplace_back( get_action( N(eosio.token), N(transfer),
                                               vector<permission_level>{{manager, config::active_name}},
                                               mutable_variant_object()
                                               ("from",     manager)
                                               ("to",       to )
                                               ("quantity", amount )
                                               ("memo",     "")
                                               )
                                   );
      }
      set_transaction_headers( trx );
      trx.sign( get_private_key( manager, "active" ), control->get_chain_id()  );
      push_transaction( trx );
   }

   double stake2votes( const asset& stake ) const {
      auto now = control->pending_block_time().time_since_epoch().count() / 1000000;
      return stake.get_amount() * pow(2, int64_t((now - (config::block_timestamp_epoch / 1000)) / (86400 * 7))/ double(52) ); // 52 week periods (i.e. ~years)
   }

   double stake2votes( const string& s ) const {
      return stake2votes( STRSYM(s) );
   }

   fc::variant get_stats( const string& symbolname ) const {
      auto symb = eosio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;
      vector<char> data = get_row_by_account( N(eosio.token), symbol_code, N(stat), symbol_code );
      return data.empty() ? fc::variant() : token_abi_ser.binary_to_variant( "currency_stats", data, abi_serializer_max_time );
   }

   asset get_token_supply() const {
      return get_stats("4," CORE_SYM_NAME)["supply"].as<asset>();
   }

   int64_t get_activated_share() const {
      const int64_t active_stake = get_global_state()["active_stake"].as<int64_t>();
      return 100 * active_stake / get_token_supply().get_amount(); // voting.cpp
   }

   size_t active_producers_num() const {
      return control->active_producers().producers.size();
   }

   uint32_t head_block_num() const {
      return control->head_block_num();
   }

   uint64_t microseconds_since_epoch_of_iso_string( const fc::variant& v ) {
      return static_cast<uint64_t>( time_point::from_iso_string( v.as_string() ).time_since_epoch().count() );
   }

#define GET_GLOBAL_STATE_FUNC(function, account, abi_type) \
   fc::variant function() const { \
      vector<char> data = \
         get_row_by_account( config::system_account_name, config::system_account_name, N(account), N(account) ); \
      if (data.empty()) { \
         std::cout << "\nData is empty\n"; \
      } \
      return data.empty() \
         ? fc::variant() \
         : abi_ser.binary_to_variant( #abi_type, data, abi_serializer_max_time ); \
   }

   GET_GLOBAL_STATE_FUNC(get_global_state,  global,  eosio_global_state)
   GET_GLOBAL_STATE_FUNC(get_global_state2, global2, eosio_global_state2)
   GET_GLOBAL_STATE_FUNC(get_global_state3, global3, eosio_global_state3)
   GET_GLOBAL_STATE_FUNC(get_global_state4, global4, eosio_global_state4)

#undef GET_GLOBAL_STATE_FUNC

   fc::variant get_refund_request( name account ) {
      vector<char> data = get_row_by_account( config::system_account_name, account, N(refunds), account );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "refund_request", data, abi_serializer_max_time );
   }

#ifndef NDEBUG
   fc::variant get_dlogs() const {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name, N(dlogs), N(dlogs) );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "dlogs", data, abi_serializer_max_time );
   }
#endif // NDEBUG

   void print_debug_logs() const {
#ifndef NDEBUG
      std::string dlog;
      for (const auto& log : get_dlogs()["data"].as<std::vector<std::string>>()) {
         dlog += "  " + log + "\n";
      }
      BOOST_TEST_MESSAGE("debug log:\n" + dlog);
#endif // NDEBUG
   }

   abi_serializer initialize_multisig() {
      abi_serializer msig_abi_ser;
      {
         create_account_with_resources( N(eosio.msig), config::system_account_name );
         BOOST_REQUIRE_EQUAL( success(), buyram( "eosio", "eosio.msig", STRSYM("5000.0000") ) );
         produce_block();

         auto trace = base_tester::push_action(config::system_account_name, N(setpriv),
                                               config::system_account_name,  mutable_variant_object()
                                               ("account", "eosio.msig")
                                               ("is_priv", 1)
         );

         set_code( N(eosio.msig), contracts::msig_wasm() );
         set_abi( N(eosio.msig), contracts::msig_abi().data() );

         produce_blocks();
         const auto& accnt = control->db().get<account_object,by_name>( N(eosio.msig) );
         abi_def msig_abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, msig_abi), true);
         msig_abi_ser.set_abi(msig_abi, abi_serializer_max_time);
      }
      return msig_abi_ser;
   }

   vector<name> active_and_vote_producers() {
      //stake more than 15% of total EOS supply to activate chain
      transfer( "eosio", "alice1111111", STRSYM("75271872.0000"), "eosio" );
      BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("25090624.0000"), STRSYM("25090624.0000"), STRSYM("25090624.0000") ) );

      // create accounts {defproducera, defproducerb, ..., defproducerz} and register as producers
      std::vector<account_name> producer_names;
      {
         producer_names.reserve('z' - 'a' + 1);
         const std::string root("defproducer");
         for ( char c = 'a'; c < 'a'+21; ++c ) {
            producer_names.emplace_back(root + std::string(1, c));
         }
         setup_producer_accounts(producer_names);
         for (const auto& p: producer_names) {

            BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
         }
      }
      produce_blocks( 250);

      auto trace_auth = TESTER::push_action(config::system_account_name, updateauth::get_name(), config::system_account_name, mvo()
                                            ("account", name(config::system_account_name).to_string())
                                            ("permission", name(config::active_name).to_string())
                                            ("parent", name(config::owner_name).to_string())
                                            ("auth",  authority(1, {key_weight{get_public_key( config::system_account_name, "active" ), 1}}, {
                                                  permission_level_weight{{config::system_account_name, config::eosio_code_name}, 1},
                                                     permission_level_weight{{config::producers_account_name,  config::active_name}, 1}
                                               }
                                            ))
      );
      BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace_auth->receipt->status);

      const std::string voter_root("producvoter");
      auto voter_balance = STRSYM("2860000.0000");
      auto vote_stake = STRSYM("1430000.0000");
      auto ram_stake = STRSYM("1430000.0000");
      for (auto i = 0; i < 21; ++i) {
         auto voter = account_name(std::string(voter_root + (char)('a' + i)));
         create_account_with_resources(voter, config::system_account_name);
         transfer(config::system_account_name, voter, voter_balance, config::system_account_name);
         BOOST_REQUIRE_EQUAL(success(), stake( voter, STRSYM("0.0000"), STRSYM("0.0000"), vote_stake) );
         BOOST_REQUIRE_EQUAL(success(), buyram( voter, voter, ram_stake ) );
         BOOST_REQUIRE_EQUAL(success(), push_action(voter, N(voteproducer), mvo()
                                                    ("voter",  voter)
                                                    ("proxy", name(0).to_string())
                                                    ("producers", vector<account_name>{ producer_names[i] })
                             )
         );
      }
      produce_blocks( 700 );

      auto producer_keys = control->head_block_state()->active_schedule.producers;
      BOOST_REQUIRE_EQUAL( 21, producer_keys.size() );
      BOOST_REQUIRE_EQUAL( name("defproducera"), producer_keys[0].producer_name );

      return producer_names;
   }

   asset cross_15_percent_threshold() {
      const asset vote_15_percent = STRSYM("25090625.0000");
      setup_producer_accounts({N(producer1111)});
      regproducer(N(producer1111));
      {
         signed_transaction trx;
         set_transaction_headers(trx);

         trx.actions.emplace_back( get_action( config::system_account_name, N(delegatebw),
                                               vector<permission_level>{{config::system_account_name, config::active_name}},
                                               mvo()
                                               ("from", name{config::system_account_name})
                                               ("receiver", "producer1111")
                                               ("stake_net_quantity", STRSYM("0.0000"))
                                               ("stake_cpu_quantity", STRSYM("0.0000"))
                                               ("stake_vote_quantity", vote_15_percent)
                                               ("transfer", true) ) );
         trx.actions.emplace_back( get_action( config::system_account_name, N(voteproducer),
                                               vector<permission_level>{{N(producer1111), config::active_name}},
                                               mvo()
                                               ("voter", "producer1111")
                                               ("proxy", name(0).to_string())
                                               ("producers", vector<account_name>(1, N(producer1111))) ) );
         trx.actions.emplace_back( get_action( config::system_account_name, N(undelegatebw),
                                               vector<permission_level>{{N(producer1111), config::active_name}},
                                               mvo()
                                               ("from", "producer1111")
                                               ("receiver", "producer1111")
                                               ("unstake_net_quantity", STRSYM("0.0000"))
                                               ("unstake_cpu_quantity", STRSYM("0.0000"))
                                               ("unstake_vote_quantity", vote_15_percent) ) );

         set_transaction_headers(trx);
         trx.sign( get_private_key( config::system_account_name, "active" ), control->get_chain_id() );
         trx.sign( get_private_key( N(producer1111), "active" ), control->get_chain_id() );
         push_transaction( trx );
         produce_block();
      }
      return vote_15_percent;
   }

   abi_serializer abi_ser;
   abi_serializer token_abi_ser;
};

inline fc::mutable_variant_object voter( account_name acct ) {
   return mutable_variant_object()
      ("owner", acct)
      ("proxy", name(0).to_string())
      ("producers", variants() )
      ("staked", int64_t(0))
      //("last_vote_weight", double(0))
      ("proxied_vote_weight", double(0))
      ("is_proxy", 0)
      ;
}

inline fc::mutable_variant_object voter( account_name acct, const asset& vote_stake ) {
   return voter( acct )( "staked", vote_stake.get_amount() );
}

inline fc::mutable_variant_object voter( account_name acct, int64_t vote_stake ) {
   return voter( acct )( "staked", vote_stake );
}

inline fc::mutable_variant_object proxy( account_name acct ) {
   return voter( acct )( "is_proxy", 1 );
}

inline uint64_t M( const string& eos_str ) {
   return STRSYM( eos_str ).get_amount();
}

/// Generate n different producer names of length (prefix.size()+suffix.size()).
/// Prefix is constant, suffix is variable.
/// Only the following symbols allowed: ".12345abcdefghijklmnopqrstuvwxyz".
inline std::vector<account_name> generate_names(size_t             n,
                                                const std::string& prefix = "pp",
                                                const std::string& suffix = "12345a") {
   std::vector<account_name> v;
   std::string msuffix = suffix;

   // check n is not too big
   static const std::vector<size_t> factorials = {1, 2, 6, 24, 120, 720, 5040, 40320, 362880};
   assert(0 < suffix.size() && suffix.size() <=factorials.size() &&
          0 < n && n < factorials[suffix.size()-1]);

   for (size_t i = 0; i < n; i++) {
      v.emplace_back(account_name{prefix + msuffix});
      std::next_permutation(std::begin(msuffix), std::end(msuffix));
   }
   return v;
}

inline std::string variant_to_string(const fc::variant& v) {
   std::stringstream ss;
   ss << v;
   return ss.str();
}

} // namespace eosio_system
