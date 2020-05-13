#include "eosio.system_tester.hpp"

#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/wast_to_wasm.hpp>
#include <fc/log/logger.hpp>
#include <wasm-jit/Runtime/Runtime.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <typeinfo>

//XXX: run tests with --log_level=message (or below) to see BOOST_TEST_MESSAGE output
// e.g.:
//   ./cicd/build.sh --build-tests --build-type Debug && \
//      ./build/tests/unit_test -p -l all -r detailed -t eosio_system_tests/stake_validators_correlation -- --verbose ; echo $?

static constexpr uint32_t seconds_per_year      = 52 * 7 * 24 * 3600;
static constexpr uint32_t seconds_per_day       = 24 * 3600;
static constexpr uint32_t seconds_per_hour      = 3600;
static constexpr int64_t  useconds_per_year     = int64_t(seconds_per_year) * 1000'000ll;
static constexpr int64_t  useconds_per_day      = int64_t(seconds_per_day) * 1000'000ll;
static constexpr int64_t  useconds_per_hour     = int64_t(seconds_per_hour) * 1000'000ll;
static constexpr uint32_t blocks_per_day        = 2 * seconds_per_day; // half seconds per day
static constexpr uint32_t blocks_per_hour       = 2 * 3600;

static constexpr int64_t min_producer_activated_stake = 30'000'0000;

struct _abi_hash {
   name owner;
   fc::sha256 hash;
};
FC_REFLECT( _abi_hash, (owner)(hash) );

struct connector {
   asset balance;
   double weight = .5;
};
FC_REFLECT( connector, (balance)(weight) );

using namespace eosio_system;

namespace bad = boost::adaptors;
namespace bal = boost::algorithm;

BOOST_AUTO_TEST_SUITE(eosio_system_tests)

static bool within_error(int64_t a, int64_t b, int64_t err) { return std::abs(a - b) <= err; };
static bool within_one(int64_t a, int64_t b) { return within_error(a, b, 1); }

static double get_target_emission_per_year(double activated_share) {
   if (activated_share <= 0.33) {
      return 0.2;
   } else if (activated_share >= 0.66) {
      return 0.1;
   }
   return -10. / 33 * (activated_share - 0.33) + 0.2;
}

static double get_continuous_rate(double emission_rate) {
   return (pow(1 + emission_rate, 1./blocks_per_hour) - 1) * blocks_per_hour;
}

fc::mutable_variant_object config_to_variant( const eosio::chain::chain_config& config ) {
   return mutable_variant_object()
      ( "max_block_net_usage", config.max_block_net_usage )
      ( "target_block_net_usage_pct", config.target_block_net_usage_pct )
      ( "max_transaction_net_usage", config.max_transaction_net_usage )
      ( "base_per_transaction_net_usage", config.base_per_transaction_net_usage )
      ( "context_free_discount_net_usage_num", config.context_free_discount_net_usage_num )
      ( "context_free_discount_net_usage_den", config.context_free_discount_net_usage_den )
      ( "max_block_cpu_usage", config.max_block_cpu_usage )
      ( "target_block_cpu_usage_pct", config.target_block_cpu_usage_pct )
      ( "max_transaction_cpu_usage", config.max_transaction_cpu_usage )
      ( "min_transaction_cpu_usage", config.min_transaction_cpu_usage )
      ( "max_transaction_lifetime", config.max_transaction_lifetime )
      ( "deferred_trx_expiration_window", config.deferred_trx_expiration_window )
      ( "max_transaction_delay", config.max_transaction_delay )
      ( "max_inline_action_size", config.max_inline_action_size )
      ( "max_inline_action_depth", config.max_inline_action_depth )
      ( "max_authority_depth", config.max_authority_depth );
}


BOOST_FIXTURE_TEST_CASE( buysell, eosio_system_tester ) try {

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );

   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "eosio", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );

   auto total = get_total_stake( "alice1111111" );
   auto init_bytes =  total["ram_bytes"].as_uint64();

   const asset initial_ram_balance = get_balance(N(eosio.ram));
   const asset initial_ramfee_balance = get_balance(N(eosio.ramfee));
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("200.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("800.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( initial_ram_balance + STRSYM("199.0000"), get_balance(N(eosio.ram)) );
   BOOST_REQUIRE_EQUAL( initial_ramfee_balance + STRSYM("1.0000"), get_balance(N(eosio.ramfee)) );

   total = get_total_stake( "alice1111111" );
   auto bytes = total["ram_bytes"].as_uint64();
   auto bought_bytes = bytes - init_bytes;
   wdump((init_bytes)(bought_bytes)(bytes) );

   BOOST_REQUIRE_EQUAL( true, 0 < bought_bytes );

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", bought_bytes ) );
   BOOST_REQUIRE_EQUAL( STRSYM("998.0049"), get_balance( "alice1111111" ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( true, total["ram_bytes"].as_uint64() == init_bytes );

   transfer( "eosio", "alice1111111", STRSYM("100000000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( STRSYM("100000998.0049"), get_balance( "alice1111111" ) );
   // alice buys ram for 10000000.0000, 0.5% = 50000.0000 go to ramfee
   // after fee 9950000.0000 go to bought bytes
   // when selling back bought bytes, pay 0.5% fee and get back 99.5% of 9950000.0000 = 9900250.0000
   // expected account after that is 90000998.0049 + 9900250.0000 = 99901248.0049 with a difference
   // of order 0.0001 due to rounding errors
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("90000998.0049"), get_balance( "alice1111111" ) );

   total = get_total_stake( "alice1111111" );
   bytes = total["ram_bytes"].as_uint64();
   bought_bytes = bytes - init_bytes;
   wdump((init_bytes)(bought_bytes)(bytes) );

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", bought_bytes ) );
   total = get_total_stake( "alice1111111" );

   bytes = total["ram_bytes"].as_uint64();
   bought_bytes = bytes - init_bytes;
   wdump((init_bytes)(bought_bytes)(bytes) );

   BOOST_REQUIRE_EQUAL( true, total["ram_bytes"].as_uint64() == init_bytes );
   BOOST_REQUIRE_EQUAL( STRSYM("99901248.0048"), get_balance( "alice1111111" ) );

   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("30.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("99900688.0048"), get_balance( "alice1111111" ) );

   auto newtotal = get_total_stake( "alice1111111" );

   auto newbytes = newtotal["ram_bytes"].as_uint64();
   bought_bytes = newbytes - bytes;
   wdump((newbytes)(bytes)(bought_bytes) );

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", bought_bytes ) );
   BOOST_REQUIRE_EQUAL( STRSYM("99901242.4187"), get_balance( "alice1111111" ) );

   newtotal = get_total_stake( "alice1111111" );
   auto startbytes = newtotal["ram_bytes"].as_uint64();

   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("10000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("300000.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("49301242.4187"), get_balance( "alice1111111" ) );

   auto finaltotal = get_total_stake( "alice1111111" );
   auto endbytes = finaltotal["ram_bytes"].as_uint64();

   bought_bytes = endbytes - startbytes;
   wdump((startbytes)(endbytes)(bought_bytes) );

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", bought_bytes ) );

   BOOST_REQUIRE_EQUAL( false, get_row_by_account( config::system_account_name, config::system_account_name,
                                                   N(rammarket), symbol{SY(4,RAMCORE)}.value() ).empty() );

   auto get_ram_market = [this]() -> fc::variant {
      vector<char> data = get_row_by_account( config::system_account_name, config::system_account_name,
                                              N(rammarket), symbol{SY(4,RAMCORE)}.value() );
      BOOST_REQUIRE( !data.empty() );
      return abi_ser.binary_to_variant("exchange_state", data, abi_serializer_max_time);
   };

   {
      transfer( config::system_account_name, "alice1111111", STRSYM("10000000.0000"), config::system_account_name );
      uint64_t bytes0 = get_total_stake( "alice1111111" )["ram_bytes"].as_uint64();

      auto market = get_ram_market();
      const asset r0 = market["base"].as<connector>().balance;
      const asset e0 = market["quote"].as<connector>().balance;
      BOOST_REQUIRE_EQUAL( asset::from_string("0 RAM").get_symbol(),     r0.get_symbol() );
      BOOST_REQUIRE_EQUAL( STRSYM("0.0000").get_symbol(), e0.get_symbol() );

      const asset payment = STRSYM("10000000.0000");
      BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", payment ) );
      uint64_t bytes1 = get_total_stake( "alice1111111" )["ram_bytes"].as_uint64();

      const int64_t fee = (payment.get_amount() + 199) / 200;
      const double net_payment = payment.get_amount() - fee;
      const int64_t expected_delta = net_payment * r0.get_amount() / ( net_payment + e0.get_amount() );

      BOOST_REQUIRE_EQUAL( expected_delta, bytes1 -  bytes0 );
   }

   {
      transfer( config::system_account_name, "bob111111111", STRSYM("100000.0000"), config::system_account_name );
      BOOST_REQUIRE_EQUAL( wasm_assert_msg("must reserve a positive amount"),
                           buyrambytes( "bob111111111", "bob111111111", 1 ) );

      uint64_t bytes0 = get_total_stake( "bob111111111" )["ram_bytes"].as_uint64();
      BOOST_REQUIRE_EQUAL( success(), buyrambytes( "bob111111111", "bob111111111", 1024 ) );
      uint64_t bytes1 = get_total_stake( "bob111111111" )["ram_bytes"].as_uint64();
      BOOST_REQUIRE( within_one( 1024, bytes1 - bytes0 ) );

      BOOST_REQUIRE_EQUAL( success(), buyrambytes( "bob111111111", "bob111111111", 1024 * 1024) );
      uint64_t bytes2 = get_total_stake( "bob111111111" )["ram_bytes"].as_uint64();
      BOOST_REQUIRE( within_one( 1024 * 1024, bytes2 - bytes1 ) );
   }

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_unstake, eosio_system_tester ) try {
   cross_15_percent_threshold();

   produce_blocks( 10 );
   produce_block( fc::hours(3*24) );

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );

   BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( success(), stake( "eosio", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );

   auto total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());

   const auto init_eosio_stake_balance = get_balance( N(eosio.stake) );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( init_eosio_stake_balance + STRSYM("400.0000"), get_balance( N(eosio.stake) ) );
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   produce_block( fc::hours(14*24-1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( init_eosio_stake_balance + STRSYM("400.0000"), get_balance( N(eosio.stake) ) );
   //after 3 days funds should be released
   produce_block( fc::hours(1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( init_eosio_stake_balance, get_balance( N(eosio.stake) ) );

   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance( "alice1111111" ) );
   total = get_total_stake("bob111111111");
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());

   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000").get_amount(), total["net_weight"].as<asset>().get_amount() );
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000").get_amount(), total["cpu_weight"].as<asset>().get_amount() );
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000").get_amount(), total["vote_weight"].as<asset>().get_amount() );

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000")), get_voter_info( "alice1111111" ) );

   auto bytes = total["ram_bytes"].as_uint64();
   BOOST_REQUIRE_EQUAL( true, 0 < bytes );

   //unstake from bob111111111
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", "bob111111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );
   total = get_total_stake("bob111111111");
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   produce_block( fc::hours(14*24-1) ); // DAOBET
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance( "alice1111111" ) );
   //after 3 days funds should be released
   produce_block( fc::hours(1) );
   produce_blocks(1);

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance( "alice1111111" ) );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_unstake_with_transfer, eosio_system_tester ) try {
   cross_15_percent_threshold();

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );

   //eosio stakes for alice with transfer flag

   transfer( "eosio", "bob111111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake_with_transfer( "bob111111111", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

   //check that alice has both bandwidth and voting power
   auto total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("100.0000")), get_voter_info( "alice1111111" ) );

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );

   //alice stakes for herself
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   //now alice's stake should be equal to transfered from eosio + own stake
   total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("410.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("200.0000")), get_voter_info( "alice1111111" ) );

   //alice can unstake everything (including what was transfered)
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("400.0000"), STRSYM("200.0000"), STRSYM("200.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   produce_block( fc::hours(14*24-1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   //after 14 days funds should be released

   produce_block( fc::hours(1) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( STRSYM("1400.0000"), get_balance( "alice1111111" ) );

   //stake should be equal to what was staked in constructor, voting power should be 0
   total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000")), get_voter_info( "alice1111111" ) );

   // Now alice stakes to bob with transfer flag
   BOOST_REQUIRE_EQUAL( success(), stake_with_transfer( "alice1111111", "bob111111111", STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( stake_to_self_with_transfer, eosio_system_tester ) try {
   cross_15_percent_threshold();

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("cannot use transfer flag if delegating to self"),
      stake_with_transfer( "alice1111111", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") )
   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_while_pending_refund, eosio_system_tester ) try {
   cross_15_percent_threshold();

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );

   //eosio stakes for alice with transfer flag
   transfer( "eosio", "bob111111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake_with_transfer( "bob111111111", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

   //check that alice has both bandwidth and voting power
   auto total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("100.0000")), get_voter_info( "alice1111111" ) );

   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );

   //alice stakes for herself
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   //now alice's stake should be equal to transfered from eosio + own stake
   total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("410.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("200.0000")), get_voter_info( "alice1111111" ) );

   //alice can unstake everything (including what was transfered)
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("400.0000"), STRSYM("200.0000"), STRSYM("200.0000") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   produce_block( fc::hours(14*24-1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
   //after 14 days funds should be released

   produce_block( fc::hours(1) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( STRSYM("1400.0000"), get_balance( "alice1111111" ) );

   //stake should be equal to what was staked in constructor, voting power should be 0
   total = get_total_stake("alice1111111");
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["cpu_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000")), get_voter_info( "alice1111111" ) );

   // Now alice stakes to bob with transfer flag
   BOOST_REQUIRE_EQUAL( success(), stake_with_transfer( "alice1111111", "bob111111111", STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( fail_without_auth, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );

   BOOST_REQUIRE_EQUAL( success(), stake( "eosio", "alice1111111", STRSYM("2000.0000"), STRSYM("1000.0000"), STRSYM("0.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("10.0000"), STRSYM("10.0000"), STRSYM("0.0000") ) );

   BOOST_REQUIRE_EQUAL( error("missing authority of alice1111111"),
                        push_action( N(alice1111111), N(delegatebw), mvo()
                                    ("from",     "alice1111111")
                                    ("receiver", "bob111111111")
                                    ("stake_net_quantity", STRSYM("10.0000"))
                                    ("stake_cpu_quantity", STRSYM("10.0000"))
                                    ("stake_vote_quantity", STRSYM("0.0000"))
                                    ("transfer", true )
                                    ,false
                        )
   );

   BOOST_REQUIRE_EQUAL( error("missing authority of alice1111111"),
                        push_action(N(alice1111111), N(undelegatebw), mvo()
                                    ("from",     "alice1111111")
                                    ("receiver", "bob111111111")
                                    ("unstake_net_quantity", STRSYM("200.0000"))
                                    ("unstake_cpu_quantity", STRSYM("100.0000"))
                                    ("unstake_vote_quantity", STRSYM("0.0000"))
                                    ("transfer", false )
                                    ,false
                        )
   );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_negative, eosio_system_tester ) try {
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must stake a positive amount"),
                        stake( "alice1111111", STRSYM("-0.0001"), STRSYM("0.0000"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must stake a positive amount"),
                        stake( "alice1111111", STRSYM("0.0000"), STRSYM("-0.0001"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must stake a positive amount"),
                        stake( "alice1111111", STRSYM("0.0000"), STRSYM("-0.0001"), STRSYM("-1.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must stake a positive amount"),
                        stake( "alice1111111", STRSYM("00.0000"), STRSYM("00.0000"), STRSYM("00.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must stake a positive amount"),
                        stake( "alice1111111", STRSYM("0.0000"), STRSYM("00.0000"), STRSYM("00.0000") )

   );

   BOOST_REQUIRE_EQUAL( true, get_voter_info( "alice1111111" ).is_null() );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( unstake_negative, eosio_system_tester ) try {
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );

   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0001"), STRSYM("100.0001"), STRSYM("100.0001") ) );

   auto vinfo = get_voter_info("alice1111111" );
   wdump((vinfo));
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("100.0001") ), get_voter_info( "alice1111111" ) );


   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must unstake a positive amount"),
                        unstake( "alice1111111", STRSYM("-1.0000"), STRSYM("0.0000"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must unstake a positive amount"),
                        unstake( "alice1111111", STRSYM("0.0000"), STRSYM("-1.0000"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must unstake a positive amount"),
                        unstake( "alice1111111", STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("-1.0000") )
   );

   //unstake all zeros
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("must unstake a positive amount"),
                        unstake( "alice1111111", STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("0.0000") )

   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( unstake_more_than_at_stake, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

   auto total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());

   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   //trying to unstake more net bandwith than at stake

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked net bandwidth"),
                        unstake( "alice1111111", STRSYM("200.0001"), STRSYM("0.0000"), STRSYM("100.0000") )
   );

   //trying to unstake more cpu bandwith than at stake
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked cpu bandwidth"),
                        unstake( "alice1111111", STRSYM("0.0000"), STRSYM("100.0001"), STRSYM("100.0000") )

   );

   //check that nothing has changed
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( delegate_to_another_user, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );

   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );

   auto total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance( "alice1111111" ) );
   //all voting power goes to alice1111111
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   //but not to bob111111111
   BOOST_REQUIRE_EQUAL( true, get_voter_info( "bob111111111" ).is_null() );

   //bob111111111 should not be able to unstake what was staked by alice1111111
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked cpu bandwidth"),
                        unstake( "bob111111111", STRSYM("0.0000"), STRSYM("10.0000"), STRSYM("10.0000") )

   );
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked net bandwidth"),
                        unstake( "bob111111111", STRSYM("10.0000"), STRSYM("0.0000"), STRSYM("10.0000") )
   );

   issue_and_transfer( "carol1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", "bob111111111", STRSYM("20.0000"), STRSYM("10.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("230.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("120.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("970.0000"), get_balance( "carol1111111" ) );
   REQUIRE_MATCHING_OBJECT( voter( "carol1111111", STRSYM("0.0000") ), get_voter_info( "carol1111111" ) );

   //alice1111111 should not be able to unstake money staked by carol1111111

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked net bandwidth"),
                        unstake( "alice1111111", "bob111111111", STRSYM("201.0000"), STRSYM("0.0000"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked cpu bandwidth"),
                        unstake( "alice1111111", "bob111111111", STRSYM("0.0000"), STRSYM("101.0000"), STRSYM("0.0000") )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("insufficient staked vote bandwidth"),
      unstake( "alice1111111", "bob111111111", STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("101.0000") )

   );

   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("230.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("120.0000"), total["cpu_weight"].as<asset>());
   //balance should not change after unsuccessfull attempts to unstake
   BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance( "alice1111111" ) );
   //voting power too
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   REQUIRE_MATCHING_OBJECT( voter( "carol1111111", STRSYM("0.0000") ), get_voter_info( "carol1111111" ) );
   BOOST_REQUIRE_EQUAL( true, get_voter_info( "bob111111111" ).is_null() );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_unstake_separate, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance( "alice1111111" ) );

   //everything at once
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("10.0000"), STRSYM("20.0000"), STRSYM("10.0000") ) );
   auto total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("20.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("30.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["vote_weight"].as<asset>());

   //cpu
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("100.0000"), STRSYM("0.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("120.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("30.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["vote_weight"].as<asset>());

   //net
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("0.0000"), STRSYM("200.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("120.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("230.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["vote_weight"].as<asset>());

   //vote
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("100.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("120.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("230.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["vote_weight"].as<asset>());

   //unstake cpu
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("100.0000"), STRSYM("0.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("20.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("230.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["vote_weight"].as<asset>());

   //unstake net
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("0.0000"), STRSYM("200.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("20.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("30.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["vote_weight"].as<asset>());

   //unstake vote
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("100.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("20.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("30.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["vote_weight"].as<asset>());
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( adding_stake_partial_unstake, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );

   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("100.0000"), STRSYM("50.0000"), STRSYM("0.0000") ) );

   auto total = get_total_stake( "bob111111111" );

   BOOST_REQUIRE_EQUAL( STRSYM("310.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("160.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("550.0000"), get_balance( "alice1111111" ) );

   //unstake a share
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", "bob111111111", STRSYM("150.0000"), STRSYM("75.0000"), STRSYM("00.0000") ) );

   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("160.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("85.0000"), total["cpu_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );

   //unstake more
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", "bob111111111", STRSYM("50.0000"), STRSYM("25.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("60.0000"), total["cpu_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );

   //combined amount should be available only in 14 days
   produce_block( fc::days(13) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("550.0000"), get_balance( "alice1111111" ) );
   produce_block( fc::days(1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( STRSYM("850.0000"), get_balance( "alice1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_from_refund, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

   auto total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"),  total["vote_weight"].as<asset>());

   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("50.0000"), STRSYM("50.0000"), STRSYM("0.0000") ) );

   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("60.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("60.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("100.0000") ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("500.0000"), get_balance( "alice1111111" ) );

   //unstake a share
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("100.0000"), STRSYM("50.0000"), STRSYM("100.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("60.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("500.0000"), get_balance( "alice1111111" ) );
   auto refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("50.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["vote_amount"].as<asset>() );
   //XXX auto request_time = refund["request_time"].as_int64();

   //alice delegates to bob, should pull from liquid balance not refund
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("50.0000"), STRSYM("50.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("60.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("0.0000") ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("400.0000"), get_balance( "alice1111111" ) );
   refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM( "50.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM( "100.0000"), refund["vote_amount"].as<asset>() );

   //stake less than pending refund, entire amount should be taken from refund
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("50.0000"), STRSYM("25.0000"), STRSYM("25.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("160.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("85.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("25.0000"), total["vote_weight"].as<asset>());
   refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("50.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("25.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("75.0000"), refund["vote_amount"].as<asset>() );
   //request time should stay the same
   //BOOST_REQUIRE_EQUAL( request_time, refund["request_time"].as_int64() );
   //balance should stay the same
   BOOST_REQUIRE_EQUAL( STRSYM("400.0000"), get_balance( "alice1111111" ) );

   //stake exactly pending refund amount
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("50.0000"), STRSYM("25.0000"), STRSYM("75.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), total["vote_weight"].as<asset>());
   //pending refund should be removed
   refund = get_refund_request( "alice1111111" );
   BOOST_TEST_REQUIRE( refund.is_null() );
   //balance should stay the same
   BOOST_REQUIRE_EQUAL( STRSYM("400.0000"), get_balance( "alice1111111" ) );

   //create pending refund again
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("10.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("400.0000"), get_balance( "alice1111111" ) );
   refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["vote_amount"].as<asset>() );

   //stake more than pending refund
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("300.0000"), STRSYM("200.0000"), STRSYM("200.0000") ) );
   total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("310.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), total["vote_weight"].as<asset>());
   // vote_weight + delegated to bob (50 + 50)
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("200.0000") ), get_voter_info( "alice1111111" ) );
   refund = get_refund_request( "alice1111111" );
   BOOST_TEST_REQUIRE( refund.is_null() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), get_balance( "alice1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_to_another_user_not_from_refund, eosio_system_tester ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );

   auto total = get_total_stake( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   REQUIRE_MATCHING_OBJECT( voter( "alice1111111", STRSYM("100.0000") ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("600.0000"), get_balance( "alice1111111" ) );

   //unstake
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("100.0000") ) );
   auto refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["vote_amount"].as<asset>() );

   //stake to another user
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "bob111111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );
   total = get_total_stake( "bob111111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("210.0000"), total["net_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("110.0000"), total["cpu_weight"].as<asset>());
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), total["vote_weight"].as<asset>());

   //stake should be taken from alices' balance, and refund request should stay the same
   BOOST_REQUIRE_EQUAL( STRSYM("300.0000"), get_balance( "alice1111111" ) );
   refund = get_refund_request( "alice1111111" );
   BOOST_REQUIRE_EQUAL( STRSYM("200.0000"), refund["net_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["cpu_amount"].as<asset>() );
   BOOST_REQUIRE_EQUAL( STRSYM("100.0000"), refund["vote_amount"].as<asset>() );

} FC_LOG_AND_RETHROW()


// Tests for voting
BOOST_FIXTURE_TEST_CASE( producer_register_unregister, eosio_system_tester ) try {
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );

   //fc::variant params = producer_parameters_example(1);
   auto key =  fc::crypto::public_key( std::string("EOS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV") );
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", key )
                                               ("url", "http://block.one")
                                               ("location", 1)
                        )
   );

   auto info = get_producer_info( "alice1111111" );
   BOOST_REQUIRE_EQUAL( "alice1111111", info["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( 0, info["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( "http://block.one", info["url"].as_string() );

   //change parameters one by one to check for things like #3783
   //fc::variant params2 = producer_parameters_example(2);
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", key )
                                               ("url", "http://block.two")
                                               ("location", 1)
                        )
   );
   info = get_producer_info( "alice1111111" );
   BOOST_REQUIRE_EQUAL( "alice1111111", info["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( key, fc::crypto::public_key(info["producer_key"].as_string()) );
   BOOST_REQUIRE_EQUAL( "http://block.two", info["url"].as_string() );
   BOOST_REQUIRE_EQUAL( 1, info["location"].as_int64() );

   auto key2 =  fc::crypto::public_key( std::string("EOS5jnmSKrzdBHE9n8hw58y7yxFWBC8SNiG7m8S1crJH3KvAnf9o6") );
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", key2 )
                                               ("url", "http://block.two")
                                               ("location", 2)
                        )
   );
   info = get_producer_info( "alice1111111" );
   BOOST_REQUIRE_EQUAL( "alice1111111", info["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( key2, fc::crypto::public_key(info["producer_key"].as_string()) );
   BOOST_REQUIRE_EQUAL( "http://block.two", info["url"].as_string() );
   BOOST_REQUIRE_EQUAL( 2, info["location"].as_int64() );

   //unregister producer
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(unregprod), mvo()
                                               ("producer",  "alice1111111")
                        )
   );
   info = get_producer_info( "alice1111111" );
   //key should be empty
   BOOST_REQUIRE_EQUAL( fc::crypto::public_key(), fc::crypto::public_key(info["producer_key"].as_string()) );
   //everything else should stay the same
   BOOST_REQUIRE_EQUAL( "alice1111111", info["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( 0, info["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( "http://block.two", info["url"].as_string() );

   //unregister bob111111111 who is not a producer
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "producer not found" ),
                        push_action( N(bob111111111), N(unregprod), mvo()
                                     ("producer",  "bob111111111")
                        )
   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( vote_for_producer, eosio_system_tester, * boost::unit_test::tolerance(1e+5) ) try {
   cross_15_percent_threshold();

   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   fc::variant params = producer_parameters_example(1);
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", get_public_key( N(alice1111111), "active") )
                                               ("url", "http://daobet.org")
                                               ("location", 0 )
                        )
   );
   auto prod = get_producer_info( "alice1111111" );
   BOOST_REQUIRE_EQUAL( "alice1111111", prod["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( 0, prod["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( "http://daobet.org", prod["url"].as_string() );

   issue_and_transfer( "bob111111111", STRSYM("2000.0000"),  config::system_account_name );
   issue_and_transfer( "carol1111111", STRSYM("3000.0000"),  config::system_account_name );

   //bob111111111 makes stake
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("11.0000"), STRSYM("0.1111"), STRSYM("11.1111") ) );
   BOOST_REQUIRE_EQUAL( STRSYM("1977.7778"), get_balance( "bob111111111" ) );
   REQUIRE_MATCHING_OBJECT( voter( "bob111111111", STRSYM("11.1111") ), get_voter_info( "bob111111111" ) );

   //bob111111111 votes for alice1111111
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), { N(alice1111111) } ) );

   //check that producer parameters stay the same after voting
   prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("11.1111")) == prod["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( "alice1111111", prod["owner"].as_string() );
   BOOST_REQUIRE_EQUAL( "http://daobet.org", prod["url"].as_string() );

   //carol1111111 makes stake
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", STRSYM("22.0000"), STRSYM("0.2222"), STRSYM("22.2222") ) );
   REQUIRE_MATCHING_OBJECT( voter( "carol1111111", STRSYM("22.2222") ), get_voter_info( "carol1111111" ) );
   BOOST_REQUIRE_EQUAL( STRSYM("2955.5556"), get_balance( "carol1111111" ) );
   //carol1111111 votes for alice1111111
   BOOST_REQUIRE_EQUAL( success(), vote( N(carol1111111), { N(alice1111111) } ) );

   //new stake votes be added to alice1111111's total_votes
   prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("33.3333")) == prod["total_votes"].as_double() );

   //bob111111111 increases his stake
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("33.0000"), STRSYM("0.3333"), STRSYM("33.3333") ) );
   //alice1111111 stake with transfer to bob111111111
   BOOST_REQUIRE_EQUAL( success(), stake_with_transfer( "alice1111111", "bob111111111", STRSYM("22.0000"), STRSYM("0.2222"), STRSYM("22.2222") ) );
   //should increase alice1111111's total_votes
   prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("88.8888")) == prod["total_votes"].as_double() );

   //carol1111111 unstakes part of the stake
   BOOST_REQUIRE_EQUAL( success(), unstake( "carol1111111", STRSYM("2.0000"), STRSYM("0.0002")/*"2.0000 EOS", "0.0002 EOS"*/, STRSYM("2.0002") ) );

   //should decrease alice1111111's total_votes
   prod = get_producer_info( "alice1111111" );
   wdump((prod));
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("86.8886")) == prod["total_votes"].as_double() );

   //bob111111111 revokes his vote
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), vector<account_name>() ) );

   //should decrease alice1111111's total_votes
   prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("20.2220")) == prod["total_votes"].as_double() );
   //but eos should still be at stake
   BOOST_REQUIRE_EQUAL( STRSYM("1911.1112"), get_balance( "bob111111111" ) );

   //carol1111111 unstakes rest of eos
   BOOST_REQUIRE_EQUAL( success(), unstake( "carol1111111", STRSYM("20.0000"), STRSYM("0.2220"), STRSYM("20.2220") ) );
   //should decrease alice1111111's total_votes to zero
   prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( 0.0 == prod["total_votes"].as_double() );

   //carol1111111 should receive funds in 14 days
   produce_block( fc::days(14) );
   produce_block();
   BOOST_REQUIRE_EQUAL( STRSYM("3000.0000"), get_balance( "carol1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( unregistered_producer_voting, eosio_system_tester, * boost::unit_test::tolerance(1e+5) ) try {
   issue_and_transfer( "bob111111111", STRSYM("2000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("13.0000"), STRSYM("1.0000"), STRSYM("13.5791") ) );
   REQUIRE_MATCHING_OBJECT( voter( "bob111111111", STRSYM("13.5791") ), get_voter_info( "bob111111111" ) );

   //bob111111111 should not be able to vote for alice1111111 who is not a producer
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "producer alice1111111 is not registered" ),
                        vote( N(bob111111111), { N(alice1111111) } ) );

   //alice1111111 registers as a producer
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   fc::variant params = producer_parameters_example(1);
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", get_public_key( N(alice1111111), "active") )
                                               ("url", "")
                                               ("location", 0)
                        )
   );
   //and then unregisters
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(unregprod), mvo()
                                               ("producer",  "alice1111111")
                        )
   );
   //key should be empty
   auto prod = get_producer_info( "alice1111111" );
   BOOST_REQUIRE_EQUAL( fc::crypto::public_key(), fc::crypto::public_key(prod["producer_key"].as_string()) );

   //bob111111111 should not be able to vote for alice1111111 who is an unregistered producer
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "producer alice1111111 is not currently registered" ),
                        vote( N(bob111111111), { N(alice1111111) } ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( more_than_1_producer_voting, eosio_system_tester ) try { // DAOBET
   issue_and_transfer( "bob111111111", STRSYM("2000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("13.0000"), STRSYM("0.5791"), STRSYM("13.5791") ) );
   REQUIRE_MATCHING_OBJECT( voter( "bob111111111", STRSYM("13.5791") ), get_voter_info( "bob111111111" ) );

   //bob111111111 should not be able to vote for alice1111111 who is not a producer
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "attempt to vote for too many producers" ),
                        vote( N(bob111111111), vector<account_name>(2, N(alice1111111)) ) );

} FC_LOG_AND_RETHROW()


//DAOBET: test deleted:
// BOOST_FIXTURE_TEST_CASE( vote_same_producer_30_times, eosio_system_tester ) try {
// } FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( producer_keep_votes, eosio_system_tester, * boost::unit_test::tolerance(1e+5) ) try {
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   fc::variant params = producer_parameters_example(1);
   vector<char> key = fc::raw::pack( get_public_key( N(alice1111111), "active" ) );
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", get_public_key( N(alice1111111), "active") )
                                               ("url", "")
                                               ("location", 0)
                        )
   );

   //bob111111111 makes stake
   issue_and_transfer( "bob111111111", STRSYM("2000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("10.0000"), STRSYM("10.0000"), STRSYM("13.5791") ) );
   REQUIRE_MATCHING_OBJECT( voter( "bob111111111", STRSYM("13.5791") ), get_voter_info( "bob111111111" ) );

   //bob111111111 votes for alice1111111
   BOOST_REQUIRE_EQUAL( success(), vote(N(bob111111111), { N(alice1111111) } ) );

   auto prod = get_producer_info( "alice1111111" );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("13.5791")) == prod["total_votes"].as_double() );

   //unregister producer
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(unregprod), mvo()
                                               ("producer",  "alice1111111")
                        )
   );
   prod = get_producer_info( "alice1111111" );
   //key should be empty
   BOOST_REQUIRE_EQUAL( fc::crypto::public_key(), fc::crypto::public_key(prod["producer_key"].as_string()) );
   //check parameters just in case
   //REQUIRE_MATCHING_OBJECT( params, prod["prefs"]);
   //votes should stay the same
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("13.5791")), prod["total_votes"].as_double() );

   //regtister the same producer again
   params = producer_parameters_example(2);
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", get_public_key( N(alice1111111), "active") )
                                               ("url", "")
                                               ("location", 0)
                        )
   );
   prod = get_producer_info( "alice1111111" );
   //votes should stay the same
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("13.5791")), prod["total_votes"].as_double() );

   //change parameters
   params = producer_parameters_example(3);
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproducer), mvo()
                                               ("producer",  "alice1111111")
                                               ("producer_key", get_public_key( N(alice1111111), "active") )
                                               ("url","")
                                               ("location", 0)
                        )
   );
   prod = get_producer_info( "alice1111111" );
   //votes should stay the same
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("13.5791")), prod["total_votes"].as_double() );
   //check parameters just in case
   //REQUIRE_MATCHING_OBJECT( params, prod["prefs"]);

} FC_LOG_AND_RETHROW()


//DAOBET: test deleted:
// BOOST_FIXTURE_TEST_CASE( vote_for_two_producers, eosio_system_tester, * boost::unit_test::tolerance(1e+5) ) try {
// } FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( proxy_register_unregister_keeps_stake, eosio_system_tester ) try {
   //register proxy by first action for this user ever
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(regproxy), mvo()
                                               ("proxy",  "alice1111111")
                                               ("isproxy", true )
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" ), get_voter_info( "alice1111111" ) );

   //unregister proxy
   BOOST_REQUIRE_EQUAL( success(), push_action(N(alice1111111), N(regproxy), mvo()
                                               ("proxy",  "alice1111111")
                                               ("isproxy", false)
                        )
   );
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111" ), get_voter_info( "alice1111111" ) );

   //stake and then register as a proxy
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("200.0002"), STRSYM("100.0001"), STRSYM("1.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), push_action( N(bob111111111), N(regproxy), mvo()
                                               ("proxy",  "bob111111111")
                                               ("isproxy", true)
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "bob111111111" )( "staked", 10000 ), get_voter_info( "bob111111111" ) );
   //unrgister and check that stake is still in place
   BOOST_REQUIRE_EQUAL( success(), push_action( N(bob111111111), N(regproxy), mvo()
                                               ("proxy",  "bob111111111")
                                               ("isproxy", false)
                        )
   );
   REQUIRE_MATCHING_OBJECT( voter( "bob111111111", STRSYM("1.0000") ), get_voter_info( "bob111111111" ) );

   //register as a proxy and then stake
   BOOST_REQUIRE_EQUAL( success(), push_action( N(carol1111111), N(regproxy), mvo()
                                               ("proxy",  "carol1111111")
                                               ("isproxy", true)
                        )
   );
   issue_and_transfer( "carol1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", STRSYM("246.0002"), STRSYM("531.0001"), STRSYM("42.0000") ) );
   //check that both proxy flag and stake a correct
   REQUIRE_MATCHING_OBJECT( proxy( "carol1111111" )( "staked", 420000 ), get_voter_info( "carol1111111" ) );

   //unregister
   BOOST_REQUIRE_EQUAL( success(), push_action( N(carol1111111), N(regproxy), mvo()
                                                ("proxy",  "carol1111111")
                                                ("isproxy", false)
                        )
   );
   REQUIRE_MATCHING_OBJECT( voter( "carol1111111", STRSYM("42.0000") ), get_voter_info( "carol1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( proxy_stake_unstake_keeps_proxy_flag, eosio_system_tester ) try {
   cross_15_percent_threshold();

   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                               ("proxy",  "alice1111111")
                                               ("isproxy", true)
                        )
   );
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" ), get_voter_info( "alice1111111" ) );

   //stake
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("100.0000"), STRSYM("50.0000"), STRSYM("1.0000") ) );
   //check that account is still a proxy
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "staked", 10000 ), get_voter_info( "alice1111111" ) );

   //stake more
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("30.0000"), STRSYM("20.0000"), STRSYM("1.0000") ) );
   //check that account is still a proxy
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )("staked", 20000 ), get_voter_info( "alice1111111" ) );

   //unstake more
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("65.0000"), STRSYM("35.0000"), STRSYM("1.0000") ) );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )("staked", 10000 ), get_voter_info( "alice1111111" ) );

   //unstake the rest
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("65.0000"), STRSYM("35.0000"), STRSYM("1.0000") ) );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "staked", 0 ), get_voter_info( "alice1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( proxy_actions_affect_producers, eosio_system_tester, * boost::unit_test::tolerance(1e+5) ) try {
   cross_15_percent_threshold();

   create_accounts_with_resources( {  N(defproducer1), N(defproducer2), N(defproducer3) } );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer1" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer2" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer3" ) );

   //register as a proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy", true)
                        )
   );

   //accumulate proxied votes
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("150.0003") ) );
   BOOST_REQUIRE_EQUAL( success(), vote(N(bob111111111), vector<account_name>(), N(alice1111111) ) );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "proxied_vote_weight", stake2votes(STRSYM("150.0003")) ), get_voter_info( "alice1111111" ) );

   //vote for producers
   BOOST_REQUIRE_EQUAL( success(), vote(N(alice1111111), { N(defproducer1) } ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("150.0003")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("0.0000")) == get_producer_info( "defproducer2" )["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( 0 == get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //vote for another producers
   BOOST_REQUIRE_EQUAL( success(), vote( N(alice1111111), { N(defproducer2) } ) );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3")["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("150.0003")) == get_producer_info( "defproducer2" )["total_votes"].as_double() );

   //unregister proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy", false)
                        )
   );
   //REQUIRE_MATCHING_OBJECT( voter( "alice1111111" )( "proxied_vote_weight", stake2votes(STRSYM("150.0003")) ), get_voter_info( "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer2" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //register proxy again
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy", true)
                        )
   );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("0.0000")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("150.0003")) == get_producer_info( "defproducer2" )["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("0.0000")) == get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //stake increase by proxy itself affects producers
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("30.0001"), STRSYM("20.0001"), STRSYM("50.0002") ) );
   BOOST_REQUIRE_EQUAL( stake2votes(STRSYM("200.0005")), get_producer_info( "defproducer2" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );
   ///@{
   ///DAOBET
   // BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer2" )["total_votes"].as_double() );
   // BOOST_REQUIRE_EQUAL( stake2votes(STRSYM("200.0005")), get_producer_info( "defproducer3" )["total_votes"].as_double() );
   ///@}

   //stake decrease by proxy itself affects producers
   BOOST_REQUIRE_EQUAL( success(), unstake( "alice1111111", STRSYM("10.0001"), STRSYM("10.0001"), STRSYM("20.0002") ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("180.0003")) == get_producer_info( "defproducer2" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

} FC_LOG_AND_RETHROW()


//DAOBET: token emission depending of activated_share (#24)
BOOST_FIXTURE_TEST_CASE( token_emission, eosio_system_tester, * boost::unit_test::tolerance(1e-3) ) try {
   cross_15_percent_threshold();

   create_accounts_with_resources( {  N(defproducer1) } );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer1") );

   transfer( config::system_account_name, "alice1111111", STRSYM("130000000.0000"), config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "alice1111111", STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("120000000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(alice1111111), { N(defproducer1) } ) );

   // First case with activated_share >= 0.66 (-> emission = 0.1)
   auto initial_global_state = get_global_state();
   asset initial_supply = get_token_supply();
   double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE ( 0.66 <= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount() );
   BOOST_REQUIRE_EQUAL(0.1, emission_rate);

   // Second case with 0.33 <= activated_share <= 0.66 (-> 0.1 <= emission <= 0.2)

   auto claim_every_day = [&](int years = 1) {
      for(int i = 0; i < years * 365; i++) {
         produce_block(fc::days(1));
         BOOST_REQUIRE_EQUAL( success(), push_action("defproducer1", N(claimrewards), mvo()("owner", "defproducer1") ) );
      }
   };

   claim_every_day(1);

   initial_global_state = get_global_state();
   auto initial_supply_after = initial_supply.get_amount() * (1 + emission_rate);
   initial_supply = get_token_supply();
   BOOST_TEST_REQUIRE(1.0 == initial_supply_after / initial_supply.get_amount());

   emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE ( 0.66 >= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount() &&
                   0.33 <= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE(0.1 <= emission_rate && 0.2 >= emission_rate);

   // Third case with 0.33 <= activated_share <= 0.66 (-> 0.1 <= emission <= 0.2)
   produce_block(fc::days(2 * 365));
   BOOST_REQUIRE_EQUAL( success(), push_action( "defproducer1", N(claimrewards), mvo()("owner", "defproducer1") ) );
   initial_global_state = get_global_state();
   initial_supply = get_token_supply();

   emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE ( 0.66 >= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount() &&
                   0.33 <= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE(0.1 <= emission_rate && 0.2 >= emission_rate);

   // Fourth case with 0.33 >= activated_share (-> emission = 0.2)
   produce_block(fc::days(7 * 365));
   BOOST_REQUIRE_EQUAL( success(), push_action( "defproducer1", N(claimrewards), mvo()("owner", "defproducer1") ) );
   initial_global_state = get_global_state();
   initial_supply = get_token_supply();

   initial_global_state = get_global_state();
   initial_supply = get_token_supply();
   emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
   BOOST_REQUIRE ( 0.33 >= 1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount() );
   BOOST_REQUIRE_EQUAL(0.2, emission_rate);

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(producer_pay, eosio_system_tester, * boost::unit_test::tolerance(1e-2)) try {

   const asset large_asset = core_sym::from_int(min_producer_activated_stake / 2);
   create_account_with_resources( N(defproducera), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );
   create_account_with_resources( N(defproducerb), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );
   create_account_with_resources( N(defproducerc), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );

   create_account_with_resources( N(producvotera), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );
   create_account_with_resources( N(producvoterb), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );

   BOOST_REQUIRE_EQUAL(success(), regproducer(N(defproducera)));
   produce_block(fc::hours(24));

   auto prod = get_producer_info( N(defproducera) );
   wdump((prod));
   BOOST_REQUIRE_EQUAL("defproducera", prod["owner"].as_string());
   BOOST_REQUIRE_EQUAL(0, prod["total_votes"].as_double());

   ///@{
   ///DAOBET
   transfer( config::system_account_name, "producvotera", STRSYM("60000000.0000"), config::system_account_name);
   BOOST_REQUIRE_EQUAL(success(), stake("producvotera", STRSYM("10.0000"), STRSYM("10.0000"), STRSYM("30000000.0000")));
   BOOST_REQUIRE_EQUAL(success(), vote( N(producvotera), { N(defproducera) }));
   ///@}

   // defproducera is the only active producer
   // produce enough blocks so new schedule kicks in and defproducera produces some blocks
   {
      produce_blocks(50);

      const auto     initial_global_state      = get_global_state();
      const uint64_t initial_claim_time        = microseconds_since_epoch_of_iso_string( initial_global_state["last_pervote_bucket_fill"] );
      const int64_t  initial_pervote_bucket    = initial_global_state["pervote_bucket"].as<int64_t>();
      const int64_t  initial_perblock_bucket   = initial_global_state["perblock_bucket"].as<int64_t>();
      const int64_t  initial_dao               = get_balance(N(eosio.saving)).get_amount(); // DAOBET
      const uint32_t initial_tot_unpaid_blocks = initial_global_state["total_unpaid_blocks"].as<uint32_t>();

      wdump((initial_global_state));
      wdump((get_producer_info(N(defproducera))));
      print_debug_logs();

      prod = get_producer_info("defproducera");
      const uint32_t unpaid_blocks = prod["unpaid_blocks"].as<uint32_t>();
      BOOST_REQUIRE_GT(unpaid_blocks, 1);

      BOOST_REQUIRE_EQUAL(initial_tot_unpaid_blocks, unpaid_blocks);

      const asset initial_supply  = get_token_supply();
      const asset initial_balance = get_balance(N(defproducera));
      const double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["total_activated_stake"].as<int64_t>() / initial_supply.get_amount());
      const double continuous_rate = get_continuous_rate(emission_rate);

      BOOST_REQUIRE_EQUAL(success(), push_action(N(defproducera), N(claimrewards), mvo()("owner", "defproducera")));

      const auto     global_state      = get_global_state();
      const uint64_t claim_time        = microseconds_since_epoch_of_iso_string( global_state["last_pervote_bucket_fill"] );
      const int64_t  pervote_bucket    = global_state["pervote_bucket"].as<int64_t>();
      const int64_t  perblock_bucket   = global_state["perblock_bucket"].as<int64_t>();
      const int64_t  dao               = get_balance(N(eosio.saving)).get_amount(); // DAOBET
      const uint32_t tot_unpaid_blocks = global_state["total_unpaid_blocks"].as<uint32_t>();

      prod = get_producer_info("defproducera");
      BOOST_REQUIRE_EQUAL(1, prod["unpaid_blocks"].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(1, tot_unpaid_blocks);
      const asset supply  = get_token_supply();
      const asset balance = get_balance(N(defproducera));

      BOOST_REQUIRE_EQUAL(claim_time, microseconds_since_epoch_of_iso_string( prod["last_claim_time"] ));

      auto usecs_between_fills = claim_time - initial_claim_time;
      int32_t secs_between_fills = usecs_between_fills/1000000;

      BOOST_REQUIRE_EQUAL(0, initial_dao); // DAOBET
      BOOST_REQUIRE_EQUAL(0, initial_perblock_bucket);
      BOOST_REQUIRE_EQUAL(0, initial_pervote_bucket);

      auto minted = supply.get_amount() - initial_supply.get_amount();

      BOOST_REQUIRE_EQUAL(int64_t( ( initial_supply.get_amount() * double(secs_between_fills) * continuous_rate ) / seconds_per_year ),
                          supply.get_amount() - initial_supply.get_amount());
      ///@{
      ///DAOBET
      BOOST_REQUIRE_EQUAL(int64_t( ( initial_supply.get_amount() * double(secs_between_fills) * (continuous_rate * 1 / 5.) / seconds_per_year ) ),
                          dao - initial_dao);
      BOOST_REQUIRE_EQUAL(int64_t( ( initial_supply.get_amount() * double(secs_between_fills) * (continuous_rate * 4 / 5. * 0.25) / seconds_per_year ) ),
                          balance.get_amount() - initial_balance.get_amount());

      auto to_dao = int64_t ( initial_supply.get_amount() * double(secs_between_fills) * (continuous_rate * 1 / 5.) / seconds_per_year );
      auto to_producers = minted - to_dao;
      ///@}

      int64_t from_perblock_bucket = to_producers / 4;
      int64_t from_pervote_bucket  = to_producers - from_perblock_bucket;


      if (from_pervote_bucket >= 100 * 10000) {
         BOOST_REQUIRE_EQUAL(from_perblock_bucket + from_pervote_bucket, balance.get_amount() - initial_balance.get_amount());
         BOOST_REQUIRE_EQUAL(0, pervote_bucket);
      } else {
         BOOST_REQUIRE_EQUAL(from_perblock_bucket, balance.get_amount() - initial_balance.get_amount());
         BOOST_REQUIRE_EQUAL(from_pervote_bucket, pervote_bucket);
      }
   }

   {
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("already claimed rewards within past day"),
                          push_action(N(defproducera), N(claimrewards), mvo()("owner", "defproducera")));
   }

   // defproducera waits for 23 hours and 55 minutes, can't claim rewards yet
   {
      produce_block(fc::seconds(23 * 3600 + 55 * 60));
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("already claimed rewards within past day"),
                          push_action(N(defproducera), N(claimrewards), mvo()("owner", "defproducera")));
   }

   // wait 5 more minutes, defproducera can now claim rewards again
   {
      produce_block(fc::seconds(5 * 60));

      const auto     initial_global_state      = get_global_state();
      const uint64_t initial_claim_time        = microseconds_since_epoch_of_iso_string( initial_global_state["last_pervote_bucket_fill"] );
      const int64_t  initial_pervote_bucket    = initial_global_state["pervote_bucket"].as<int64_t>();
      const int64_t  initial_perblock_bucket   = initial_global_state["perblock_bucket"].as<int64_t>();
      const int64_t  initial_dao               = get_balance(N(eosio.saving)).get_amount(); // DAOBET
      const uint32_t initial_tot_unpaid_blocks = initial_global_state["total_unpaid_blocks"].as<uint32_t>();
      const double   initial_tot_vote_weight   = initial_global_state["total_producer_vote_weight"].as<double>();

      prod = get_producer_info("defproducera");
      const uint32_t unpaid_blocks = prod["unpaid_blocks"].as<uint32_t>();
      BOOST_REQUIRE(1 < unpaid_blocks);
      BOOST_REQUIRE_EQUAL(initial_tot_unpaid_blocks, unpaid_blocks);
      BOOST_REQUIRE(0 < prod["total_votes"].as<double>());
      BOOST_TEST(initial_tot_vote_weight, prod["total_votes"].as<double>());
      BOOST_REQUIRE(0 < microseconds_since_epoch_of_iso_string( prod["last_claim_time"] ));

      BOOST_REQUIRE_EQUAL(initial_tot_unpaid_blocks, unpaid_blocks);

      const asset initial_supply  = get_token_supply();
      const asset initial_balance = get_balance(N(defproducera));
      const double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["total_activated_stake"].as<int64_t>() / initial_supply.get_amount());
      const double continuous_rate = get_continuous_rate(emission_rate);

      BOOST_REQUIRE_EQUAL(success(), push_action(N(defproducera), N(claimrewards), mvo()("owner", "defproducera")));

      const auto global_state          = get_global_state();
      const uint64_t claim_time        = microseconds_since_epoch_of_iso_string( global_state["last_pervote_bucket_fill"] );
      const int64_t  pervote_bucket    = global_state["pervote_bucket"].as<int64_t>();
      const int64_t  perblock_bucket   = global_state["perblock_bucket"].as<int64_t>();
      const int64_t  dao               = get_balance(N(eosio.saving)).get_amount(); // DAOBET
      const uint32_t tot_unpaid_blocks = global_state["total_unpaid_blocks"].as<uint32_t>();

      prod = get_producer_info("defproducera");
      BOOST_REQUIRE_EQUAL(1, prod["unpaid_blocks"].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(1, tot_unpaid_blocks);
      const asset supply  = get_token_supply();
      const asset balance = get_balance(N(defproducera));

      BOOST_REQUIRE_EQUAL(claim_time, microseconds_since_epoch_of_iso_string( prod["last_claim_time"] ));
      auto usecs_between_fills = claim_time - initial_claim_time;
      auto to_dao = dao - initial_dao;

      ///@{
      ///DAOBET
      BOOST_REQUIRE_EQUAL(int64_t( ( double(initial_supply.get_amount()) * double(usecs_between_fills) * continuous_rate / useconds_per_year ) ),
                          supply.get_amount() - initial_supply.get_amount());
      BOOST_REQUIRE_EQUAL( (supply.get_amount() - initial_supply.get_amount()) / 5,
                          to_dao);

      int64_t to_producer        = supply.get_amount() - initial_supply.get_amount() - to_dao;
      ///@}
      int64_t to_perblock_bucket = to_producer / 4;
      int64_t to_pervote_bucket  = to_producer - to_perblock_bucket;

      if (to_pervote_bucket + initial_pervote_bucket >= 100 * 10000) {
         BOOST_REQUIRE_EQUAL(to_producer + initial_pervote_bucket - pervote_bucket, balance.get_amount() - initial_balance.get_amount());
      } else {
         BOOST_REQUIRE_EQUAL(to_perblock_bucket, balance.get_amount() - initial_balance.get_amount());
         BOOST_REQUIRE_EQUAL(to_pervote_bucket + initial_pervote_bucket, pervote_bucket);
      }
   }

   // defproducerb tries to claim rewards but he's not on the list
   {
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("unable to find key"),
                          push_action(N(defproducerb), N(claimrewards), mvo()("owner", "defproducerb")));
   }

   // test stability over a year
   {
      regproducer(N(defproducerb));
      regproducer(N(defproducerc));
      produce_block(fc::hours(24));
      const asset   initial_supply  = get_token_supply();
      const int64_t initial_dao = get_balance(N(eosio.saving)).get_amount(); // DAOBET
      const auto    initial_global_state = get_global_state();

      const double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());

      for (uint32_t i = 0; i < 7 * 52; ++i) {
         produce_block(fc::seconds(8 * 3600));
         BOOST_REQUIRE_EQUAL(success(), push_action(N(defproducerc), N(claimrewards), mvo()("owner", "defproducerc")));
         produce_block(fc::seconds(8 * 3600));
         BOOST_REQUIRE_EQUAL(success(), push_action(N(defproducerb), N(claimrewards), mvo()("owner", "defproducerb")));
         produce_block(fc::seconds(8 * 3600));
         BOOST_REQUIRE_EQUAL(success(), push_action(N(defproducera), N(claimrewards), mvo()("owner", "defproducera")));
      }
      const asset   supply  = get_token_supply();
      const int64_t dao = get_balance(N(eosio.saving)).get_amount(); // DAOBET

      auto after_year = int64_t(double(initial_supply.get_amount()) * (1 + emission_rate));
      BOOST_REQUIRE(1e-3 > 1.0 * abs(after_year - supply.get_amount()) / initial_supply.get_amount());
   }
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(multiple_producer_pay, eosio_system_tester, * boost::unit_test::tolerance(1e-10)) try {

   const asset net = STRSYM("80.0000");
   const asset cpu = STRSYM("80.0000");

   // create 40 accounts {defproducera, defproducerb, ..., defproducerz, abcproducera, ..., defproducern} and register as producers
   std::vector<account_name> producer_names;
   std::vector<account_name> voter_names;
   {
      producer_names.reserve('z' - 'a' + 1);
      voter_names.reserve('z' - 'a' + 1);
      {
         const std::string root("defproducer");
         const std::string voter_root("producvoter");
         for ( char c = 'a'; c <= 'z'; ++c ) {
            producer_names.emplace_back(root + std::string(1, c));
            voter_names.emplace_back(voter_root + std::string(1, c));
         }
      }
      {
         const std::string root("abcproducer");
         const std::string voter_root("absvoteracc");
         for ( char c = 'a'; c <= 'n'; ++c ) {
            producer_names.emplace_back(root + std::string(1, c));
            voter_names.emplace_back(voter_root + std::string(1, c));
         }
      }
      BOOST_REQUIRE_EQUAL( producer_names.size(), 40 );
      BOOST_REQUIRE_EQUAL( voter_names.size(), 40 );

      setup_producer_accounts(producer_names);
      for (const auto& p: producer_names) {
         BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
         produce_blocks(1);
         wdump((p));
         BOOST_TEST(0 == get_producer_info(p)["total_votes"].as<double>());
      }
   }

   const int64_t mul = TOKEN_FRACTIONAL_PART_MULTIPLIER;
   const auto vote_stake_a_to_j = 3'000'000 * mul;
   const auto vote_stake_k_to_u = 2'000'000 * mul;
   const auto vote_stake_rest   = 1'000'000 * mul;

   // producvotera .. producvoterj
   for (auto i = 0; i < 10; ++i) {
      auto v = voter_names[i];
      create_account_with_resources( v, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
      transfer( config::system_account_name, v, core_sym::from_int(vote_stake_a_to_j), config::system_account_name );
      BOOST_REQUIRE_EQUAL(success(), stake(v, STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("3000000.0000")) );
   }
   // producvoterk .. producvoteru
   for (auto i = 10; i < 21; ++i) {
      auto v = voter_names[i];
      create_account_with_resources( v, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
      transfer( config::system_account_name, v, core_sym::from_int(vote_stake_k_to_u), config::system_account_name );
      BOOST_REQUIRE_EQUAL(success(), stake(v, STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("2000000.0000")) );
   }
   // producvoterv .. producvoterz && absvoteracca .. absvoteraccn
   for (auto i = 21; i < voter_names.size(); ++i) {
      auto v = voter_names[i];
      create_account_with_resources( v, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
      transfer( config::system_account_name, v, core_sym::from_int(vote_stake_rest), config::system_account_name );
      BOOST_REQUIRE_EQUAL(success(), stake(v, STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("1000000.0000")) );
   }

   produce_block( fc::hours(24) );

   for (auto i = 0; i < producer_names.size(); ++i) {
      BOOST_REQUIRE_EQUAL(success(), vote(voter_names[i], { producer_names[i] }));
   }

   {
      auto proda = get_producer_info( N(defproducera) ); // vote stake = 3'000'000
      auto prodj = get_producer_info( N(defproducerj) ); // vote stake = 3'000'000
      auto prodk = get_producer_info( N(defproducerk) ); // vote stake = 2'000'000
      auto produ = get_producer_info( N(defproduceru) ); // vote stake = 2'000'000
      auto prodv = get_producer_info( N(defproducerv) ); // vote stake = 1'000'000
      auto prodz = get_producer_info( N(defproducerz) ); // vote stake = 1'000'000

      BOOST_REQUIRE (0 == proda["unpaid_blocks"].as<uint32_t>() && 0 == prodz["unpaid_blocks"].as<uint32_t>());

      // check vote ratios
      BOOST_REQUIRE ( 0 < proda["total_votes"].as<double>() && 0 < prodz["total_votes"].as<double>() );
      BOOST_TEST( proda["total_votes"].as<double>() == prodj["total_votes"].as<double>() );
      BOOST_TEST( prodk["total_votes"].as<double>() == produ["total_votes"].as<double>() );
      BOOST_TEST( prodv["total_votes"].as<double>() == prodz["total_votes"].as<double>() );
      BOOST_TEST( 2 * proda["total_votes"].as<double>() == 3 * produ["total_votes"].as<double>() );
      BOOST_TEST( 1 * proda["total_votes"].as<double>() == 3 * prodz["total_votes"].as<double>() );
   }

   // give a chance for everyone to produce blocks
   {
      produce_blocks(23 * 12 + 2000);
      bool all_21_produced = true;
      for (uint32_t i = 0; i < 21; ++i) {
         if (0 == get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            all_21_produced = false;
            BOOST_TEST_MESSAGE(i << "-th producer should produce");
         }
      }
      bool rest_didnt_produce = true;
      for (uint32_t i = 21; i < producer_names.size(); ++i) {
         if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            rest_didnt_produce = false;
            BOOST_TEST_MESSAGE(i << "-th producer should not produce");
         }
      }
      BOOST_REQUIRE_EQUAL(all_21_produced, true);
      BOOST_REQUIRE_EQUAL(rest_didnt_produce, true);
   }

   std::vector<double> vote_shares(producer_names.size());
   {
      double total_votes = 0;
      for (uint32_t i = 0; i < producer_names.size(); ++i) {
         vote_shares[i] = get_producer_info(producer_names[i])["total_votes"].as<double>();
         total_votes += vote_shares[i];
      }
      BOOST_TEST(total_votes == get_global_state()["total_producer_vote_weight"].as<double>());
      std::for_each(vote_shares.begin(), vote_shares.end(), [total_votes](double& x) { x /= total_votes; });

      BOOST_TEST(double(1) == std::accumulate(vote_shares.begin(), vote_shares.end(), double(0)));
      BOOST_TEST(double(3./71.) == vote_shares.front());
      BOOST_TEST(double(1./71.) == vote_shares.back());
   }

   {
      const uint32_t prod_index = 2;
      const auto prod_name = producer_names[prod_index];

      const auto     initial_global_state      = get_global_state();
      const uint64_t initial_claim_time        = microseconds_since_epoch_of_iso_string( initial_global_state["last_pervote_bucket_fill"] );
      const int64_t  initial_pervote_bucket    = initial_global_state["pervote_bucket"].as<int64_t>();
      const int64_t  initial_perblock_bucket   = initial_global_state["perblock_bucket"].as<int64_t>();
      const int64_t  initial_savings           = get_balance(N(eosio.saving)).get_amount();
      const uint32_t initial_tot_unpaid_blocks = initial_global_state["total_unpaid_blocks"].as<uint32_t>();
      const asset    initial_supply            = get_token_supply();
      const asset    initial_bpay_balance      = get_balance(N(eosio.bpay));
      const asset    initial_vpay_balance      = get_balance(N(eosio.vpay));
      const asset    initial_balance           = get_balance(prod_name);
      const uint32_t initial_unpaid_blocks     = get_producer_info(prod_name)["unpaid_blocks"].as<uint32_t>();

      BOOST_TEST_MESSAGE("initial:"
         << "\n  claim_time        = " << initial_claim_time
         << "\n  pervote_bucket    = " << initial_pervote_bucket
         << "\n  perblock_bucket   = " << initial_perblock_bucket
         << "\n  savings           = " << initial_savings
         << "\n  tot_unpaid_blocks = " << initial_tot_unpaid_blocks
         << "\n  supply            = " << initial_supply.to_string()
         << "\n  bpay_balance      = " << initial_bpay_balance.to_string()
         << "\n  vpay_balance      = " << initial_vpay_balance.to_string()
         << "\n  balance           = " << initial_balance.to_string()
         << "\n  unpaid_blocks     = " << initial_unpaid_blocks
      );

      ///@{
      ///DAOBET
      const double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["active_stake"].as<int64_t>() / initial_supply.get_amount());
      const double continuous_rate = get_continuous_rate(emission_rate);
      ///@}

      BOOST_REQUIRE_EQUAL(success(), push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));

      const auto     global_state      = get_global_state();
      const uint64_t claim_time        = microseconds_since_epoch_of_iso_string( global_state["last_pervote_bucket_fill"] );
      const int64_t  pervote_bucket    = global_state["pervote_bucket"].as<int64_t>();
      const int64_t  perblock_bucket   = global_state["perblock_bucket"].as<int64_t>();
      const int64_t  savings           = get_balance(N(eosio.saving)).get_amount();
      const uint32_t tot_unpaid_blocks = global_state["total_unpaid_blocks"].as<uint32_t>();
      const asset    supply            = get_token_supply();
      const asset    bpay_balance      = get_balance(N(eosio.bpay));
      const asset    vpay_balance      = get_balance(N(eosio.vpay));
      const asset    balance           = get_balance(prod_name);
      const uint32_t unpaid_blocks     = get_producer_info(prod_name)["unpaid_blocks"].as<uint32_t>();

      BOOST_TEST_MESSAGE("after claimrewards():"
         << "\n  claim_time        = " << claim_time
         << "\n  pervote_bucket    = " << pervote_bucket
         << "\n  perblock_bucket   = " << perblock_bucket
         << "\n  savings           = " << savings
         << "\n  tot_unpaid_blocks = " << tot_unpaid_blocks
         << "\n  supply            = " << supply.to_string()
         << "\n  bpay_balance      = " << bpay_balance.to_string()
         << "\n  vpay_balance      = " << vpay_balance.to_string()
         << "\n  balance           = " << balance.to_string()
         << "\n  unpaid_blocks     = " << unpaid_blocks
      );

      const uint64_t usecs_between_fills = claim_time - initial_claim_time;
      const int32_t secs_between_fills = static_cast<int32_t>(usecs_between_fills / 1'000'000);
      BOOST_TEST_MESSAGE("seconds between claims = " << secs_between_fills);

      const double expected_supply_growth = initial_supply.get_amount() * double(usecs_between_fills) * continuous_rate / useconds_per_year; // DAOBET
      BOOST_REQUIRE_EQUAL( int64_t(expected_supply_growth), supply.get_amount() - initial_supply.get_amount() );

      //TODO: also check changing emission rate after unstaking (when active_stake is decreased)

      produce_blocks(5);

      BOOST_REQUIRE_EQUAL(wasm_assert_msg("already claimed rewards within past day"),
                          push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));
   }

   {
      const uint32_t prod_index = 23;
      const auto prod_name = producer_names[prod_index];
      BOOST_REQUIRE_EQUAL(success(),
                          push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));
      BOOST_REQUIRE_EQUAL(0, get_balance(prod_name).get_amount());
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("already claimed rewards within past day"),
                          push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));
   }


   {
      const uint32_t prod_index = 24;
      const auto prod_name = producer_names[prod_index];
      BOOST_REQUIRE_EQUAL(success(),
                          push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("already claimed rewards within past day"),
                          push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)));
   }

   {
      const uint32_t rmv_index = 5;
      account_name prod_name = producer_names[rmv_index];

      auto info = get_producer_info(prod_name);
      BOOST_REQUIRE( info["is_active"].as<bool>() );
      BOOST_REQUIRE( fc::crypto::public_key() != fc::crypto::public_key(info["producer_key"].as_string()) );

      BOOST_REQUIRE_EQUAL( error("missing authority of eosio"),
                           push_action(prod_name, N(rmvproducer), mvo()("producer", prod_name)));
      BOOST_REQUIRE_EQUAL( error("missing authority of eosio"),
                           push_action(producer_names[rmv_index + 2], N(rmvproducer), mvo()("producer", prod_name) ) );
      BOOST_REQUIRE_EQUAL( success(),
                           push_action(config::system_account_name, N(rmvproducer), mvo()("producer", prod_name) ) );
      {
         bool rest_didnt_produce = true;
         for (uint32_t i = 21; i < producer_names.size(); ++i) {
            if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
               rest_didnt_produce = false;
            }
         }
         BOOST_REQUIRE(rest_didnt_produce);
      }

      produce_blocks(3 * 21 * 12);
      info = get_producer_info(prod_name);
      const uint32_t init_unpaid_blocks = info["unpaid_blocks"].as<uint32_t>();
      BOOST_REQUIRE( !info["is_active"].as<bool>() );
      BOOST_REQUIRE( fc::crypto::public_key() == fc::crypto::public_key(info["producer_key"].as_string()) );
      BOOST_REQUIRE_EQUAL( wasm_assert_msg("producer does not have an active key"),
                           push_action(prod_name, N(claimrewards), mvo()("owner", prod_name) ) );
      produce_blocks(3 * 21 * 12);
      BOOST_REQUIRE_EQUAL( init_unpaid_blocks, get_producer_info(prod_name)["unpaid_blocks"].as<uint32_t>() );
      {
         bool prod_was_replaced = false;
         for (uint32_t i = 21; i < producer_names.size(); ++i) {
            if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
               prod_was_replaced = true;
            }
         }
         BOOST_REQUIRE(prod_was_replaced);
      }
   }

   {
      BOOST_REQUIRE_EQUAL( wasm_assert_msg("producer not found"),
                           push_action( config::system_account_name, N(rmvproducer), mvo()("producer", "nonexistingp") ) );
   }

   ///@{
   ///DAOBET: Disabled because we use only revision = 0
   // produce_block(fc::hours(24));

   // // switch to new producer pay metric
   // {
   //    BOOST_REQUIRE_EQUAL( 0, get_global_state2()["revision"].as<uint8_t>() );
   //    BOOST_REQUIRE_EQUAL( error("missing authority of eosio"),
   //                         push_action(producer_names[1], N(updtrevision), mvo()("revision", 1) ) );
   //    BOOST_REQUIRE_EQUAL( success(),
   //                         push_action(config::system_account_name, N(updtrevision), mvo()("revision", 1) ) );
   //    BOOST_REQUIRE_EQUAL( 1, get_global_state2()["revision"].as<uint8_t>() );

   //    const uint32_t prod_index = 2;
   //    const auto prod_name = producer_names[prod_index];

   //    const auto     initial_prod_info         = get_producer_info(prod_name);
   //    const auto     initial_prod_info2        = get_producer_info2(prod_name);
   //    const auto     initial_global_state      = get_global_state();
   //    const double   initial_tot_votepay_share = get_global_state2()["total_producer_votepay_share"].as_double();
   //    const double   initial_tot_vpay_rate     = get_global_state3()["total_vpay_share_change_rate"].as_double();
   //    const uint64_t initial_vpay_state_update = microseconds_since_epoch_of_iso_string( get_global_state3()["last_vpay_state_update"] );
   //    const uint64_t initial_bucket_fill_time  = microseconds_since_epoch_of_iso_string( initial_global_state["last_pervote_bucket_fill"] );
   //    const int64_t  initial_pervote_bucket    = initial_global_state["pervote_bucket"].as<int64_t>();
   //    const int64_t  initial_perblock_bucket   = initial_global_state["perblock_bucket"].as<int64_t>();
   //    const uint32_t initial_tot_unpaid_blocks = initial_global_state["total_unpaid_blocks"].as<uint32_t>();
   //    const asset    initial_supply            = get_token_supply();
   //    const asset    initial_balance           = get_balance(prod_name);
   //    const uint32_t initial_unpaid_blocks     = initial_prod_info["unpaid_blocks"].as<uint32_t>();
   //    const uint64_t initial_claim_time        = microseconds_since_epoch_of_iso_string( initial_prod_info["last_claim_time"] );
   //    const uint64_t initial_prod_update_time  = microseconds_since_epoch_of_iso_string( initial_prod_info2["last_votepay_share_update"] );
   //    const int64_t  initial_dao               = get_balance(N(eosio.saving)).get_amount();

   //    const double emission_rate = get_target_emission_per_year(1.0 * initial_global_state["total_activated_stake"].as<int64_t>() / initial_supply.get_amount());
   //    const double continuous_rate = get_continuous_rate(emission_rate);


   //    BOOST_TEST_REQUIRE( 0 == get_producer_info2(prod_name)["votepay_share"].as_double() );
   //    BOOST_REQUIRE_EQUAL( success(), push_action(prod_name, N(claimrewards), mvo()("owner", prod_name) ) );

   //    const int64_t  dao               = get_balance(N(eosio.saving)).get_amount();
   //    const auto     prod_info         = get_producer_info(prod_name);
   //    const auto     prod_info2        = get_producer_info2(prod_name);
   //    const auto     global_state      = get_global_state();
   //    const uint64_t vpay_state_update = microseconds_since_epoch_of_iso_string( get_global_state3()["last_vpay_state_update"] );
   //    const uint64_t bucket_fill_time  = microseconds_since_epoch_of_iso_string( global_state["last_pervote_bucket_fill"] );
   //    const int64_t  pervote_bucket    = global_state["pervote_bucket"].as<int64_t>();
   //    const int64_t  perblock_bucket   = global_state["perblock_bucket"].as<int64_t>();
   //    const uint32_t tot_unpaid_blocks = global_state["total_unpaid_blocks"].as<uint32_t>();
   //    const asset    supply            = get_token_supply();
   //    const asset    balance           = get_balance(prod_name);
   //    const uint32_t unpaid_blocks     = prod_info["unpaid_blocks"].as<uint32_t>();
   //    const uint64_t claim_time        = microseconds_since_epoch_of_iso_string( prod_info["last_claim_time"] );
   //    const uint64_t prod_update_time  = microseconds_since_epoch_of_iso_string( prod_info2["last_votepay_share_update"] );

   //    const auto newly_minted = supply.get_amount() - initial_supply.get_amount();
   //    const auto to_dao = dao - initial_dao;

   //    const uint64_t usecs_between_fills         = bucket_fill_time - initial_bucket_fill_time;
   //    const double   secs_between_global_updates = (vpay_state_update - initial_vpay_state_update) / 1E6;
   //    const double   secs_between_prod_updates   = (prod_update_time - initial_prod_update_time) / 1E6;
   //    const double   votepay_share               = initial_prod_info2["votepay_share"].as_double() + secs_between_prod_updates * prod_info["total_votes"].as_double();
   //    const double   tot_votepay_share           = initial_tot_votepay_share + initial_tot_vpay_rate * secs_between_global_updates;

   //    const auto to_producers = newly_minted - to_dao;
   //    const int64_t expected_perblock_bucket = to_producers * 0.25;
   //    const int64_t expected_pervote_bucket  = to_producers - expected_perblock_bucket;
   //    const int64_t from_perblock_bucket = initial_unpaid_blocks * expected_perblock_bucket / initial_tot_unpaid_blocks;
   //    const int64_t from_pervote_bucket  = int64_t( ( votepay_share * expected_pervote_bucket ) / tot_votepay_share );

   //    const double expected_supply_growth = initial_supply.get_amount() * double(usecs_between_fills) * continuous_rate / usecs_per_year;
   //    BOOST_REQUIRE_EQUAL( int64_t(expected_supply_growth), supply.get_amount() - initial_supply.get_amount() );
   //    BOOST_REQUIRE_EQUAL( claim_time, vpay_state_update );
   //    // BOOST_REQUIRE( 100 * 10000 < from_pervote_bucket );
   //    BOOST_CHECK_EQUAL( expected_pervote_bucket - from_pervote_bucket, pervote_bucket );
   //    BOOST_CHECK_EQUAL( from_perblock_bucket + from_pervote_bucket, balance.get_amount() - initial_balance.get_amount() );
   //    BOOST_TEST_REQUIRE( 0 == get_producer_info2(prod_name)["votepay_share"].as_double() );

   //    produce_block(fc::hours(2));

   //    BOOST_REQUIRE_EQUAL( wasm_assert_msg("already claimed rewards within past day"),
   //                         push_action(prod_name, N(claimrewards), mvo()("owner", prod_name) ) );
   // }
   ///@}

} FC_LOG_AND_RETHROW()


///@{
///DAOBET: disable for now
// BOOST_FIXTURE_TEST_CASE(multiple_producer_votepay_share, eosio_system_tester, * boost::unit_test::tolerance(1e-10)) try {

//    const asset net = STRSYM("80.0000");
//    const asset cpu = STRSYM("80.0000");
//    const std::vector<account_name> voters = { N(producvotera), N(producvoterb), N(producvoterc), N(producvoterd) };
//    for (const auto& v: voters) {
//       create_account_with_resources( v, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
//       transfer( config::system_account_name, v, STRSYM("10000000.0000"), config::system_account_name );
//       BOOST_REQUIRE_EQUAL(success(), stake(v, STRSYM("4000000.0000"), STRSYM("4000000.0000"), STRSYM("10.0000")) );
//    }

//    // create accounts {defproducera, defproducerb, ..., defproducerz, abcproducera, ..., defproducern} and register as producers
//    std::vector<account_name> producer_names;
//    {
//       producer_names.reserve('z' - 'a' + 1);
//       {
//          const std::string root("defproducer");
//          for ( char c = 'a'; c <= 'z'; ++c ) {
//             producer_names.emplace_back(root + std::string(1, c));
//          }
//       }
//       {
//          const std::string root("abcproducer");
//          for ( char c = 'a'; c <= 'n'; ++c ) {
//             producer_names.emplace_back(root + std::string(1, c));
//          }
//       }
//       setup_producer_accounts(producer_names);
//       for (const auto& p: producer_names) {
//          BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
//          produce_blocks(1);
//          ilog( "------ get pro----------" );
//          wdump((p));
//          BOOST_TEST_REQUIRE(0 == get_producer_info(p)["total_votes"].as_double());
//          BOOST_TEST_REQUIRE(0 == get_producer_info2(p)["votepay_share"].as_double());
//          BOOST_REQUIRE(0 < microseconds_since_epoch_of_iso_string( get_producer_info2(p)["last_votepay_share_update"] ));
//       }
//    }

//    produce_block( fc::hours(24) );

//    // producvotera votes for defproducera ... defproducerj
//    // producvoterb votes for defproducera ... defproduceru
//    // producvoterc votes for defproducera ... defproducerz
//    // producvoterd votes for abcproducera ... abcproducern
//    {
//       BOOST_TEST_REQUIRE( 0 == get_global_state3()["total_vpay_share_change_rate"].as_double() );
//       BOOST_REQUIRE_EQUAL( success(), vote(N(producvotera), vector<account_name>(producer_names.begin(), producer_names.begin()+10)) );
//       produce_block( fc::hours(10) );
//       BOOST_TEST_REQUIRE( 0 == get_global_state2()["total_producer_votepay_share"].as_double() );
//       const auto& init_info  = get_producer_info(producer_names[0]);
//       const auto& init_info2 = get_producer_info2(producer_names[0]);
//       uint64_t init_update = microseconds_since_epoch_of_iso_string( init_info2["last_votepay_share_update"] );
//       double   init_votes  = init_info["total_votes"].as_double();
//       BOOST_REQUIRE_EQUAL( success(), vote(N(producvoterb), vector<account_name>(producer_names.begin(), producer_names.begin()+21)) );
//       const auto& info  = get_producer_info(producer_names[0]);
//       const auto& info2 = get_producer_info2(producer_names[0]);
//       BOOST_TEST_REQUIRE( ((microseconds_since_epoch_of_iso_string( info2["last_votepay_share_update"] ) - init_update)/double(1E6)) * init_votes == info2["votepay_share"].as_double() );
//       BOOST_TEST_REQUIRE( info2["votepay_share"].as_double() * 10 == get_global_state2()["total_producer_votepay_share"].as_double() );

//       BOOST_TEST_REQUIRE( 0 == get_producer_info2(producer_names[11])["votepay_share"].as_double() );
//       produce_block( fc::hours(13) );
//       BOOST_REQUIRE_EQUAL( success(), vote(N(producvoterc), vector<account_name>(producer_names.begin(), producer_names.begin()+26)) );
//       BOOST_REQUIRE( 0 < get_producer_info2(producer_names[11])["votepay_share"].as_double() );
//       produce_block( fc::hours(1) );
//       BOOST_REQUIRE_EQUAL( success(), vote(N(producvoterd), vector<account_name>(producer_names.begin()+26, producer_names.end())) );
//       BOOST_TEST_REQUIRE( 0 == get_producer_info2(producer_names[26])["votepay_share"].as_double() );
//    }

//    {
//       auto proda = get_producer_info( N(defproducera) );
//       auto prodj = get_producer_info( N(defproducerj) );
//       auto prodk = get_producer_info( N(defproducerk) );
//       auto produ = get_producer_info( N(defproduceru) );
//       auto prodv = get_producer_info( N(defproducerv) );
//       auto prodz = get_producer_info( N(defproducerz) );

//       BOOST_REQUIRE (0 == proda["unpaid_blocks"].as<uint32_t>() && 0 == prodz["unpaid_blocks"].as<uint32_t>());

//       // check vote ratios
//       BOOST_REQUIRE ( 0 < proda["total_votes"].as_double() && 0 < prodz["total_votes"].as_double() );
//       BOOST_TEST_REQUIRE( proda["total_votes"].as_double() == prodj["total_votes"].as_double() );
//       BOOST_TEST_REQUIRE( prodk["total_votes"].as_double() == produ["total_votes"].as_double() );
//       BOOST_TEST_REQUIRE( prodv["total_votes"].as_double() == prodz["total_votes"].as_double() );
//       BOOST_TEST_REQUIRE( 2 * proda["total_votes"].as_double() == 3 * produ["total_votes"].as_double() );
//       BOOST_TEST_REQUIRE( proda["total_votes"].as_double() ==  3 * prodz["total_votes"].as_double() );
//    }

//    std::vector<double> vote_shares(producer_names.size());
//    {
//       double total_votes = 0;
//       for (uint32_t i = 0; i < producer_names.size(); ++i) {
//          vote_shares[i] = get_producer_info(producer_names[i])["total_votes"].as_double();
//          total_votes += vote_shares[i];
//       }
//       BOOST_TEST_REQUIRE( total_votes == get_global_state()["total_producer_vote_weight"].as_double() );
//       BOOST_TEST_REQUIRE( total_votes == get_global_state3()["total_vpay_share_change_rate"].as_double() );
//       BOOST_REQUIRE_EQUAL( microseconds_since_epoch_of_iso_string( get_producer_info2(producer_names.back())["last_votepay_share_update"] ),
//                            microseconds_since_epoch_of_iso_string( get_global_state3()["last_vpay_state_update"] ) );

//       std::for_each( vote_shares.begin(), vote_shares.end(), [total_votes](double& x) { x /= total_votes; } );
//       BOOST_TEST_REQUIRE( double(1) == std::accumulate(vote_shares.begin(), vote_shares.end(), double(0)) );
//       BOOST_TEST_REQUIRE( double(3./71.) == vote_shares.front() );
//       BOOST_TEST_REQUIRE( double(1./71.) == vote_shares.back() );
//    }

//    std::vector<double> votepay_shares(producer_names.size());
//    {
//       const auto& gs3 = get_global_state3();
//       double total_votepay_shares          = 0;
//       double expected_total_votepay_shares = 0;
//       for (uint32_t i = 0; i < producer_names.size() ; ++i) {
//          const auto& info  = get_producer_info(producer_names[i]);
//          const auto& info2 = get_producer_info2(producer_names[i]);
//          votepay_shares[i] = info2["votepay_share"].as_double();
//          total_votepay_shares          += votepay_shares[i];
//          expected_total_votepay_shares += votepay_shares[i];
//          expected_total_votepay_shares += info["total_votes"].as_double()
//                                            * double( ( microseconds_since_epoch_of_iso_string( gs3["last_vpay_state_update"] )
//                                                         - microseconds_since_epoch_of_iso_string( info2["last_votepay_share_update"] )
//                                                      ) / 1E6 );
//       }
//       BOOST_TEST( expected_total_votepay_shares > total_votepay_shares );
//       BOOST_TEST_REQUIRE( expected_total_votepay_shares == get_global_state2()["total_producer_votepay_share"].as_double() );
//    }

//    {
//       const uint32_t prod_index = 15;
//       const account_name prod_name = producer_names[prod_index];
//       const auto& init_info        = get_producer_info(prod_name);
//       const auto& init_info2       = get_producer_info2(prod_name);
//       BOOST_REQUIRE( 0 < init_info2["votepay_share"].as_double() );
//       BOOST_REQUIRE( 0 < microseconds_since_epoch_of_iso_string( init_info2["last_votepay_share_update"] ) );

//       BOOST_REQUIRE_EQUAL( success(), push_action(prod_name, N(claimrewards), mvo()("owner", prod_name)) );

//       BOOST_TEST_REQUIRE( 0 == get_producer_info2(prod_name)["votepay_share"].as_double() );
//       BOOST_REQUIRE_EQUAL( get_producer_info(prod_name)["last_claim_time"].as_string(),
//                            get_producer_info2(prod_name)["last_votepay_share_update"].as_string() );
//       BOOST_REQUIRE_EQUAL( get_producer_info(prod_name)["last_claim_time"].as_string(),
//                            get_global_state3()["last_vpay_state_update"].as_string() );
//       const auto& gs3 = get_global_state3();
//       double expected_total_votepay_shares = 0;
//       for (uint32_t i = 0; i < producer_names.size(); ++i) {
//          const auto& info  = get_producer_info(producer_names[i]);
//          const auto& info2 = get_producer_info2(producer_names[i]);
//          expected_total_votepay_shares += info2["votepay_share"].as_double();
//          expected_total_votepay_shares += info["total_votes"].as_double()
//                                            * double( ( microseconds_since_epoch_of_iso_string( gs3["last_vpay_state_update"] )
//                                                         - microseconds_since_epoch_of_iso_string( info2["last_votepay_share_update"] )
//                                                      ) / 1E6 );
//       }
//       BOOST_TEST_REQUIRE( expected_total_votepay_shares == get_global_state2()["total_producer_votepay_share"].as_double() );
//    }

// } FC_LOG_AND_RETHROW()
///@}


BOOST_FIXTURE_TEST_CASE(votepay_share_invariant, eosio_system_tester, * boost::unit_test::tolerance(1e-10)) try {

   cross_15_percent_threshold();

   const asset net = STRSYM("80.0000");
   const asset cpu = STRSYM("80.0000");
   const std::vector<account_name> accounts = { N(aliceaccount), N(bobbyaccount), N(carolaccount), N(emilyaccount) };
   for (const auto& a: accounts) {
      create_account_with_resources( a, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
      transfer( config::system_account_name, a, STRSYM("1000.0000"), config::system_account_name );
   }
   const auto vota  = accounts[0];
   const auto votb  = accounts[1];
   const auto proda = accounts[2];
   const auto prodb = accounts[3];

   BOOST_REQUIRE_EQUAL( success(), stake( vota, STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("1.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( votb, STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("1.0000") ) );

   BOOST_REQUIRE_EQUAL( success(), regproducer( proda ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( prodb ) );

   BOOST_REQUIRE_EQUAL( success(), vote( vota, { proda } ) );
   BOOST_REQUIRE_EQUAL( success(), vote( votb, { prodb } ) );

   produce_block( fc::hours(25) );

   BOOST_REQUIRE_EQUAL( success(), vote( vota, { proda } ) );
   BOOST_REQUIRE_EQUAL( success(), vote( votb, { prodb } ) );

   produce_block( fc::hours(1) );

   BOOST_REQUIRE_EQUAL( success(), push_action(proda, N(claimrewards), mvo()("owner", proda)) );
   BOOST_TEST_REQUIRE( 0 == get_producer_info2(proda)["votepay_share"].as_double() );

   produce_block( fc::hours(24) );

   BOOST_REQUIRE_EQUAL( success(), vote( vota, { proda } ) );

   produce_block( fc::hours(24) );

   BOOST_REQUIRE_EQUAL( success(), push_action(prodb, N(claimrewards), mvo()("owner", prodb)) );
   BOOST_TEST_REQUIRE( 0 == get_producer_info2(prodb)["votepay_share"].as_double() );

   produce_block( fc::hours(10) );

   BOOST_REQUIRE_EQUAL( success(), vote( votb, { prodb } ) );

   produce_block( fc::hours(16) );

   BOOST_REQUIRE_EQUAL( success(), vote( votb, { prodb } ) );
   produce_block( fc::hours(2) );
   BOOST_REQUIRE_EQUAL( success(), vote( vota, { proda } ) );

   const auto& info  = get_producer_info(prodb);
   const auto& info2 = get_producer_info2(prodb);
   const auto& gs2   = get_global_state2();
   const auto& gs3   = get_global_state3();

   double expected_total_vpay_share = info2["votepay_share"].as_double()
                                       + info["total_votes"].as_double()
                                          * ( microseconds_since_epoch_of_iso_string( gs3["last_vpay_state_update"] )
                                               - microseconds_since_epoch_of_iso_string( info2["last_votepay_share_update"] ) ) / 1E6;

   BOOST_TEST_REQUIRE( expected_total_vpay_share == gs2["total_producer_votepay_share"].as_double() );

} FC_LOG_AND_RETHROW()


///@{
///DAOBET
// BOOST_FIXTURE_TEST_CASE(votepay_share_proxy, eosio_system_tester, * boost::unit_test::tolerance(1e-5)) try {

//    cross_15_percent_threshold();

//    const asset net = STRSYM("80.0000");
//    const asset cpu = STRSYM("80.0000");
//    const std::vector<account_name> accounts = { N(aliceaccount), N(bobbyaccount), N(carolaccount), N(emilyaccount) };
//    for (const auto& a: accounts) {
//       create_account_with_resources( a, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
//       transfer( config::system_account_name, a, STRSYM("1000.0000"), config::system_account_name );
//    }
//    const auto alice = accounts[0];
//    const auto bob   = accounts[1];
//    const auto carol = accounts[2];
//    const auto emily = accounts[3];

//    // alice becomes a proxy
//    BOOST_REQUIRE_EQUAL( success(), push_action( alice, N(regproxy), mvo()("proxy", alice)("isproxy", true) ) );
//    REQUIRE_MATCHING_OBJECT( proxy( alice ), get_voter_info( alice ) );

//    // carol and emily become producers
//    BOOST_REQUIRE_EQUAL( success(), regproducer( carol ) );
//    BOOST_REQUIRE_EQUAL( success(), regproducer( emily ) );

//    // bob chooses alice as proxy
//    BOOST_REQUIRE_EQUAL( success(), stake( bob, STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("1.0000") ) );
//    BOOST_REQUIRE_EQUAL( success(), stake( alice, STRSYM("150.0000"), STRSYM("150.0000"), STRSYM("1.0000") ) );
//    BOOST_REQUIRE_EQUAL( success(), vote( bob, { }, alice ) );
//    BOOST_TEST_REQUIRE( stake2votes(STRSYM("150.0003")) == get_voter_info(alice)["proxied_vote_weight"].as_double() );

//    // alice (proxy) votes for carol
//    BOOST_REQUIRE_EQUAL( success(), vote( alice, { carol } ) );
//    double total_votes = get_producer_info(carol)["total_votes"].as_double();
//    BOOST_TEST_REQUIRE( stake2votes(STRSYM("450.0003")) == total_votes );
//    BOOST_TEST_REQUIRE( 0 == get_producer_info2(carol)["votepay_share"].as_double() );
//    uint64_t last_update_time = microseconds_since_epoch_of_iso_string( get_producer_info2(carol)["last_votepay_share_update"] );

//    produce_block( fc::hours(15) );

//    // alice (proxy) votes again for carol
//    BOOST_REQUIRE_EQUAL( success(), vote( alice, { carol } ) );
//    auto cur_info2 = get_producer_info2(carol);
//    double expected_votepay_share = double( (microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] ) - last_update_time) / 1E6 ) * total_votes;
//    BOOST_TEST_REQUIRE( stake2votes(STRSYM("450.0003")) == get_producer_info(carol)["total_votes"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == cur_info2["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == get_global_state2()["total_producer_votepay_share"].as_double() );
//    last_update_time = microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] );
//    total_votes      = get_producer_info(carol)["total_votes"].as_double();

//    produce_block( fc::hours(40) );

//    // bob unstakes
//    BOOST_REQUIRE_EQUAL( success(), unstake( bob, STRSYM("10.0002"), STRSYM("10.0001"), STRSYM("1.0000") ) );
//    BOOST_TEST_REQUIRE( stake2votes(STRSYM("430.0000")), get_producer_info(carol)["total_votes"].as_double() );

//    cur_info2 = get_producer_info2(carol);
//    expected_votepay_share += double( (microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] ) - last_update_time) / 1E6 ) * total_votes;
//    BOOST_TEST_REQUIRE( expected_votepay_share == cur_info2["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == get_global_state2()["total_producer_votepay_share"].as_double() );
//    last_update_time = microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] );
//    total_votes      = get_producer_info(carol)["total_votes"].as_double();

//    // carol claims rewards
//    BOOST_REQUIRE_EQUAL( success(), push_action(carol, N(claimrewards), mvo()("owner", carol)) );

//    produce_block( fc::hours(20) );

//    // bob votes for carol
//    BOOST_REQUIRE_EQUAL( success(), vote( bob, { carol } ) );
//    BOOST_TEST_REQUIRE( stake2votes(STRSYM("430.0000")), get_producer_info(carol)["total_votes"].as_double() );
//    cur_info2 = get_producer_info2(carol);
//    expected_votepay_share = double( (microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] ) - last_update_time) / 1E6 ) * total_votes;
//    BOOST_TEST_REQUIRE( expected_votepay_share == cur_info2["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == get_global_state2()["total_producer_votepay_share"].as_double() );

//    produce_block( fc::hours(54) );

//    // bob votes for carol again
//    // carol hasn't claimed rewards in over 3 days
//    total_votes = get_producer_info(carol)["total_votes"].as_double();
//    BOOST_REQUIRE_EQUAL( success(), vote( bob, { carol } ) );
//    BOOST_REQUIRE_EQUAL( get_producer_info2(carol)["last_votepay_share_update"].as_string(),
//                         get_global_state3()["last_vpay_state_update"].as_string() );
//    BOOST_TEST_REQUIRE( 0 == get_producer_info2(carol)["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0 == get_global_state2()["total_producer_votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0 == get_global_state3()["total_vpay_share_change_rate"].as_double() );

//    produce_block( fc::hours(20) );

//    // bob votes for carol again
//    // carol still hasn't claimed rewards
//    BOOST_REQUIRE_EQUAL( success(), vote( bob, { carol } ) );
//    BOOST_REQUIRE_EQUAL(get_producer_info2(carol)["last_votepay_share_update"].as_string(),
//                        get_global_state3()["last_vpay_state_update"].as_string() );
//    BOOST_TEST_REQUIRE( 0 == get_producer_info2(carol)["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0 == get_global_state2()["total_producer_votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0 == get_global_state3()["total_vpay_share_change_rate"].as_double() );

//    produce_block( fc::hours(24) );

//    // carol finally claims rewards
//    BOOST_REQUIRE_EQUAL( success(), push_action( carol, N(claimrewards), mvo()("owner", carol) ) );
//    BOOST_TEST_REQUIRE( 0           == get_producer_info2(carol)["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0           == get_global_state2()["total_producer_votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( total_votes == get_global_state3()["total_vpay_share_change_rate"].as_double() );

//    produce_block( fc::hours(5) );

//    // alice votes for carol and emily
//    // emily hasn't claimed rewards in over 3 days
//    last_update_time = microseconds_since_epoch_of_iso_string( get_producer_info2(carol)["last_votepay_share_update"] );
//    BOOST_REQUIRE_EQUAL( success(), vote( alice, { carol, emily } ) );
//    cur_info2 = get_producer_info2(carol);
//    auto cur_info2_emily = get_producer_info2(emily);

//    expected_votepay_share = double( (microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] ) - last_update_time) / 1E6 ) * total_votes;
//    BOOST_TEST_REQUIRE( expected_votepay_share == cur_info2["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0                      == cur_info2_emily["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == get_global_state2()["total_producer_votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( get_producer_info(carol)["total_votes"].as_double() ==
//                        get_global_state3()["total_vpay_share_change_rate"].as_double() );
//    BOOST_REQUIRE_EQUAL( cur_info2["last_votepay_share_update"].as_string(),
//                         get_global_state3()["last_vpay_state_update"].as_string() );
//    BOOST_REQUIRE_EQUAL( cur_info2_emily["last_votepay_share_update"].as_string(),
//                         get_global_state3()["last_vpay_state_update"].as_string() );

//    produce_block( fc::hours(10) );

//    // bob chooses alice as proxy
//    // emily still hasn't claimed rewards
//    last_update_time = microseconds_since_epoch_of_iso_string( get_producer_info2(carol)["last_votepay_share_update"] );
//    BOOST_REQUIRE_EQUAL( success(), vote( bob, { }, alice ) );
//    cur_info2 = get_producer_info2(carol);
//    cur_info2_emily = get_producer_info2(emily);

//    expected_votepay_share += double( (microseconds_since_epoch_of_iso_string( cur_info2["last_votepay_share_update"] ) - last_update_time) / 1E6 ) * total_votes;
//    BOOST_TEST_REQUIRE( expected_votepay_share == cur_info2["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( 0                      == cur_info2_emily["votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( expected_votepay_share == get_global_state2()["total_producer_votepay_share"].as_double() );
//    BOOST_TEST_REQUIRE( get_producer_info(carol)["total_votes"].as_double() ==
//                        get_global_state3()["total_vpay_share_change_rate"].as_double() );
//    BOOST_REQUIRE_EQUAL( cur_info2["last_votepay_share_update"].as_string(),
//                         get_global_state3()["last_vpay_state_update"].as_string() );
//    BOOST_REQUIRE_EQUAL( cur_info2_emily["last_votepay_share_update"].as_string(),
//                         get_global_state3()["last_vpay_state_update"].as_string() );

// } FC_LOG_AND_RETHROW()
///@}


BOOST_FIXTURE_TEST_CASE(votepay_share_update_order, eosio_system_tester, * boost::unit_test::tolerance(1e-10)) try {

   cross_15_percent_threshold();

   const asset net = STRSYM("80.0000");
   const asset cpu = STRSYM("80.0000");
   const asset vote_stake = STRSYM("0.0000");
   const std::vector<account_name> accounts = { N(aliceaccount), N(bobbyaccount), N(carolaccount), N(emilyaccount) };
   for (const auto& a: accounts) {
      create_account_with_resources( a, config::system_account_name, STRSYM("1.0000"), false, net, cpu, vote_stake );
      transfer( config::system_account_name, a, STRSYM("1000.0000"), config::system_account_name );
   }
   const auto alice = accounts[0];
   const auto bob   = accounts[1];
   const auto carol = accounts[2];
   const auto emily = accounts[3];

   BOOST_REQUIRE_EQUAL( success(), regproducer( carol ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( emily ) );

   produce_block( fc::hours(24) );

   BOOST_REQUIRE_EQUAL( success(), stake( carol, STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("0.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( emily, STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("0.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( alice, STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("200.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( bob,   STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("200.0000") ) );

   BOOST_REQUIRE_EQUAL( success(), vote( alice, { carol } ) );
   BOOST_REQUIRE_EQUAL( success(), vote( bob, { emily } ) );


   BOOST_REQUIRE_EQUAL( success(), push_action( carol, N(claimrewards), mvo()("owner", carol) ) );
   produce_block( fc::hours(1) );
   BOOST_REQUIRE_EQUAL( success(), push_action( emily, N(claimrewards), mvo()("owner", emily) ) );

   produce_block( fc::hours(3 * 24 + 1) );

   {
      signed_transaction trx;
      set_transaction_headers(trx);

      trx.actions.emplace_back( get_action( config::system_account_name, N(claimrewards), { {carol, config::active_name} },
                                            mvo()("owner", carol) ) );

      trx.actions.emplace_back( get_action( config::system_account_name, N(voteproducer), { {alice, config::active_name} },
                                            mvo()("voter", alice)("proxy", name(0))("producers", std::vector<account_name>{ carol }) ) );

      trx.actions.emplace_back( get_action( config::system_account_name, N(voteproducer), { {bob, config::active_name} },
                                            mvo()("voter", bob)("proxy", name(0))("producers", std::vector<account_name>{ emily }) ) );

      trx.actions.emplace_back( get_action( config::system_account_name, N(claimrewards), { {emily, config::active_name} },
                                            mvo()("owner", emily) ) );

      trx.sign( get_private_key( carol, "active" ), control->get_chain_id() );
      trx.sign( get_private_key( alice, "active" ), control->get_chain_id() );
      trx.sign( get_private_key( emily, "active" ), control->get_chain_id() );
      trx.sign( get_private_key( bob, "active" ), control->get_chain_id() );

      push_transaction( trx );
   }

   const auto& carol_info  = get_producer_info(carol);
   const auto& carol_info2 = get_producer_info2(carol);
   const auto& emily_info  = get_producer_info(emily);
   const auto& emily_info2 = get_producer_info2(emily);
   const auto& gs3         = get_global_state3();
   BOOST_REQUIRE_EQUAL( carol_info2["last_votepay_share_update"].as_string(), gs3["last_vpay_state_update"].as_string() );
   BOOST_REQUIRE_EQUAL( emily_info2["last_votepay_share_update"].as_string(), gs3["last_vpay_state_update"].as_string() );
   BOOST_TEST_REQUIRE( 0  == carol_info2["votepay_share"].as_double() );
   BOOST_TEST_REQUIRE( 0  == emily_info2["votepay_share"].as_double() );
   BOOST_REQUIRE( 0 < carol_info["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( carol_info["total_votes"].as_double() == emily_info["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( gs3["total_vpay_share_change_rate"].as_double() == 2 * carol_info["total_votes"].as_double() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(votepay_transition, eosio_system_tester, * boost::unit_test::tolerance(1e-10)) try {

   const asset net = STRSYM("80.0000");
   const asset cpu = STRSYM("80.0000");
   const std::vector<account_name> voters = { N(producvotera), N(producvoterb), N(producvoterc), N(producvoterd) };
   for (const auto& v: voters) {
      create_account_with_resources( v, config::system_account_name, STRSYM("1.0000"), false, net, cpu );
      transfer( config::system_account_name, v, STRSYM("10000000.0000"), config::system_account_name );
      BOOST_REQUIRE_EQUAL(success(), stake(v, STRSYM("4000000.0000"), STRSYM("4000000.0000"), STRSYM("10.0000")) );
   }

   // create accounts {defproducera, defproducerb, ..., defproducerz} and register as producers
   std::vector<account_name> producer_names;
   {
      producer_names.reserve('z' - 'a' + 1);
      {
         const std::string root("defproducer");
         for ( char c = 'a'; c <= 'd'; ++c ) {
            producer_names.emplace_back(root + std::string(1, c));
         }
      }
      setup_producer_accounts(producer_names);
      BOOST_TEST_MESSAGE("registering producers: "
         << bal::join(producer_names | bad::transformed([](const auto& e){ return e.to_string(); }), " "));
      for (const auto& p: producer_names) {
         BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
         BOOST_TEST_REQUIRE(0 == get_producer_info(p)["total_votes"].as_double());
         BOOST_TEST_REQUIRE(0 == get_producer_info2(p)["votepay_share"].as_double());
         BOOST_REQUIRE(0 < microseconds_since_epoch_of_iso_string( get_producer_info2(p)["last_votepay_share_update"] ));
      }
   }

   BOOST_REQUIRE_EQUAL( success(), vote(N(producvotera), { producer_names[0] }) );
   auto* tbl = control->db().find<eosio::chain::table_id_object, eosio::chain::by_code_scope_table>(
                  boost::make_tuple( config::system_account_name,
                                     config::system_account_name,
                                     N(producers2) ) );
   BOOST_REQUIRE( tbl );
   BOOST_REQUIRE( 0 < microseconds_since_epoch_of_iso_string( get_producer_info2("defproducera")["last_votepay_share_update"] ) );

   // const_cast hack for now
   const_cast<chainbase::database&>(control->db()).remove( *tbl );
   tbl = control->db().find<eosio::chain::table_id_object, eosio::chain::by_code_scope_table>(
                  boost::make_tuple( config::system_account_name,
                                     config::system_account_name,
                                     N(producers2) ) );
   BOOST_REQUIRE( !tbl );

   BOOST_REQUIRE_EQUAL( success(), vote(N(producvoterb), { producer_names[0] }) );
   tbl = control->db().find<eosio::chain::table_id_object, eosio::chain::by_code_scope_table>(
            boost::make_tuple( config::system_account_name,
                               config::system_account_name,
                               N(producers2) ) );
   BOOST_REQUIRE( !tbl );
   BOOST_REQUIRE_EQUAL( success(), regproducer(N(defproducera)) );
   BOOST_REQUIRE( microseconds_since_epoch_of_iso_string( get_producer_info(N(defproducera))["last_claim_time"] ) <
      microseconds_since_epoch_of_iso_string( get_producer_info2(N(defproducera))["last_votepay_share_update"] ) );

   create_account_with_resources( N(defproducer1), config::system_account_name, STRSYM("1.0000"), false, net, cpu );
   BOOST_REQUIRE_EQUAL( success(), regproducer(N(defproducer1)) );
   BOOST_REQUIRE( 0 < microseconds_since_epoch_of_iso_string( get_producer_info(N(defproducer1))["last_claim_time"] ) );
   BOOST_REQUIRE_EQUAL( get_producer_info(N(defproducer1))["last_claim_time"].as_string(),
                        get_producer_info2(N(defproducer1))["last_votepay_share_update"].as_string() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(producers_upgrade_system_contract, eosio_system_tester) try {
   //install multisig contract
   abi_serializer msig_abi_ser = initialize_multisig();
   auto producer_names = active_and_vote_producers();

   //helper function
   auto push_action_msig = [&]( const account_name& signer, const action_name &name, const variant_object &data, bool auth = true ) -> action_result {
         string action_type_name = msig_abi_ser.get_action_type(name);

         action act;
         act.account = N(eosio.msig);
         act.name = name;
         act.data = msig_abi_ser.variant_to_binary( action_type_name, data, abi_serializer_max_time );

         return base_tester::push_action( std::move(act), auth ? uint64_t(signer) : signer == N(bob111111111) ? N(alice1111111) : N(bob111111111) );
   };
   // test begins
   vector<permission_level> prod_perms;
   for ( auto& x : producer_names ) {
      prod_perms.push_back( { name(x), config::active_name } );
   }

   transaction trx;
   {
      //prepare system contract with different hash (contract differs in one byte)
      auto code = contracts::system_wasm();
      string msg = "producer votes must be unique and sorted";
      auto it = std::search( code.begin(), code.end(), msg.begin(), msg.end() );
      BOOST_REQUIRE( it != code.end() );
      msg[0] = 'P';
      std::copy( msg.begin(), msg.end(), it );

      variant pretty_trx = fc::mutable_variant_object()
         ("expiration", "2020-01-01T00:30")
         ("ref_block_num", 2)
         ("ref_block_prefix", 3)
         ("net_usage_words", 0)
         ("max_cpu_usage_ms", 0)
         ("delay_sec", 0)
         ("actions", fc::variants({
               fc::mutable_variant_object()
                  ("account", name(config::system_account_name))
                  ("name", "setcode")
                  ("authorization", vector<permission_level>{ { config::system_account_name, config::active_name } })
                  ("data", fc::mutable_variant_object() ("account", name(config::system_account_name))
                   ("vmtype", 0)
                   ("vmversion", "0")
                   ("code", bytes( code.begin(), code.end() ))
                  )
                  })
         );
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer_max_time);
   }

   BOOST_REQUIRE_EQUAL(success(), push_action_msig( N(alice1111111), N(propose), mvo()
                                                    ("proposer",      "alice1111111")
                                                    ("proposal_name", "upgrade1")
                                                    ("trx",           trx)
                                                    ("requested", prod_perms)
                       )
   );

   // get 15 approvals
   for ( size_t i = 0; i < 14; ++i ) {
      BOOST_REQUIRE_EQUAL(success(), push_action_msig( name(producer_names[i]), N(approve), mvo()
                                                       ("proposer",      "alice1111111")
                                                       ("proposal_name", "upgrade1")
                                                       ("level",         permission_level{ name(producer_names[i]), config::active_name })
                          )
      );
   }

   //should fail
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("transaction authorization failed"),
                       push_action_msig( N(alice1111111), N(exec), mvo()
                                         ("proposer",      "alice1111111")
                                         ("proposal_name", "upgrade1")
                                         ("executer",      "alice1111111")
                       )
   );

   // one more approval
   BOOST_REQUIRE_EQUAL(success(), push_action_msig( name(producer_names[14]), N(approve), mvo()
                                                    ("proposer",      "alice1111111")
                                                    ("proposal_name", "upgrade1")
                                                    ("level",         permission_level{ name(producer_names[14]), config::active_name })
                          )
   );

   transaction_trace_ptr trace;
   control->applied_transaction.connect(
   [&]( std::tuple<const transaction_trace_ptr&, const signed_transaction&> p ) {
      const auto& t = std::get<0>(p);
      if( t->scheduled ) { trace = t; }
   } );

   BOOST_REQUIRE_EQUAL(success(), push_action_msig( N(alice1111111), N(exec), mvo()
                                                    ("proposer",      "alice1111111")
                                                    ("proposal_name", "upgrade1")
                                                    ("executer",      "alice1111111")
                       )
   );

   BOOST_REQUIRE( bool(trace) );
   BOOST_REQUIRE_EQUAL( 1, trace->action_traces.size() );
   BOOST_REQUIRE_EQUAL( transaction_receipt::executed, trace->receipt->status );

   produce_blocks( 250 );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(producer_onblock_check, eosio_system_tester) try {

   const asset large_asset = STRSYM("80.0000");
   create_account_with_resources( N(producvotera), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );
   create_account_with_resources( N(producvoterb), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );
   create_account_with_resources( N(producvoterc), config::system_account_name, STRSYM("1.0000"), false, large_asset, large_asset );

   // create accounts {defproducera, defproducerb, ..., defproducerz} and register as producers
   std::vector<account_name> producer_names;
   producer_names.reserve('z' - 'a' + 1);
   const std::string root("defproducer");
   for ( char c = 'a'; c <= 'z'; ++c ) {
      producer_names.emplace_back(root + std::string(1, c));
   }
   setup_producer_accounts(producer_names);

   for (const auto& a : producer_names) {
      regproducer(a);
   }

   produce_block(fc::hours(24));

   BOOST_REQUIRE_EQUAL(0, get_producer_info( producer_names.front() )["total_votes"].as<double>());
   BOOST_REQUIRE_EQUAL(0, get_producer_info( producer_names.back() )["total_votes"].as<double>());

   BOOST_CHECK_EQUAL( wasm_assert_msg( "cannot undelegate bandwidth until the chain is activated (at least 15% of all tokens participate in voting)" ),
                      unstake( "defproducera", STRSYM("50.0000"), STRSYM("50.0000"), STRSYM("1.0000") ) );

   for (const auto& p : producer_names) {
      vote(p, { p });
   }

   // give a chance for everyone to produce blocks
   {
      produce_blocks(21 * 12);
      bool all_21_produced = true;
      for (uint32_t i = 0; i < 21; ++i) {
         if (0 == get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            all_21_produced= false;
         }
      }
      bool rest_didnt_produce = true;
      for (uint32_t i = 21; i < producer_names.size(); ++i) {
         if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            rest_didnt_produce = false;
         }
      }
      BOOST_REQUIRE_EQUAL(false, all_21_produced);
      BOOST_REQUIRE_EQUAL(true, rest_didnt_produce);
   }

   {
      const char* claimrewards_activation_error_message = "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)";
      BOOST_CHECK_EQUAL(0, get_global_state()["total_unpaid_blocks"].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(wasm_assert_msg( claimrewards_activation_error_message ),
                          push_action(producer_names.front(), N(claimrewards), mvo()("owner", producer_names.front())));
      BOOST_REQUIRE_EQUAL(0, get_balance(producer_names.front()).get_amount());
      BOOST_REQUIRE_EQUAL(wasm_assert_msg( claimrewards_activation_error_message ),
                          push_action(producer_names.back(), N(claimrewards), mvo()("owner", producer_names.back())));
      BOOST_REQUIRE_EQUAL(0, get_balance(producer_names.back()).get_amount());
   }

   // stake across 15% boundary
   for (auto i = 0; i < 21; ++i) {
      transfer(config::system_account_name, producer_names[i], STRSYM("1.0000"));
      BOOST_REQUIRE_EQUAL(success(), stake(producer_names[i], STRSYM("0.0000"), STRSYM("0.0000"), STRSYM("1.0000")));
      BOOST_REQUIRE_EQUAL(success(), vote(producer_names[i], { producer_names[i] }));
   }

   transfer(config::system_account_name, "producvotera", STRSYM("30000200.0000"));
   BOOST_REQUIRE_EQUAL(success(), stake("producvotera", STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("30000000.0000")));
   BOOST_REQUIRE_EQUAL(success(), vote("producvotera", { producer_names.front() }));

   // give a chance for everyone to produce blocks
   {
      produce_blocks(21 * 12);
      bool all_21_produced = true;
      for (uint32_t i = 0; i < 21; ++i) {
         if (0 == get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            all_21_produced= false;
         }
      }
      bool rest_didnt_produce = true;
      for (uint32_t i = 21; i < producer_names.size(); ++i) {
         if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            rest_didnt_produce = false;
         }
      }
      BOOST_REQUIRE_EQUAL(true, all_21_produced);
      BOOST_REQUIRE_EQUAL(true, rest_didnt_produce);
      BOOST_REQUIRE_EQUAL(success(),
                          push_action(producer_names.front(), N(claimrewards), mvo()("owner", producer_names.front())));
      BOOST_REQUIRE(0 < get_balance(producer_names.front()).get_amount());
   }

   BOOST_CHECK_EQUAL( success(), unstake( "producvotera", STRSYM("50.0000"), STRSYM("50.0000"), STRSYM("1.0000") ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( voters_actions_affect_proxy_and_producers, eosio_system_tester, * boost::unit_test::tolerance(1e+6) ) try {
   cross_15_percent_threshold();

   create_accounts_with_resources( { N(donald111111), N(defproducer1), N(defproducer2), N(defproducer3) } );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer1" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer2" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer3" ) );

   //alice1111111 becomes a producer
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy", true)
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" ), get_voter_info( "alice1111111" ) );

   //alice1111111 makes stake and votes
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("30.0001"), STRSYM("20.0001"), STRSYM("50.0002") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(alice1111111), { N(defproducer1) } ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("50.0002")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   BOOST_REQUIRE_EQUAL( success(), push_action( N(donald111111), N(regproxy), mvo()
                                                ("proxy",  "donald111111")
                                                ("isproxy", true)
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "donald111111" ), get_voter_info( "donald111111" ) );

   //bob111111111 chooses alice1111111 as a proxy
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("150.0002") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), vector<account_name>(), "alice1111111" ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("150.0003")) == get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("200.0005")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //carol1111111 chooses alice1111111 as a proxy
   issue_and_transfer( "carol1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", STRSYM("30.0001"), STRSYM("20.0001"), STRSYM("30.0002") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(carol1111111), vector<account_name>(), "alice1111111" ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("200.0005")) == get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("250.0007")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //proxied voter carol1111111 increases stake
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", STRSYM("50.0000"), STRSYM("70.0000"), STRSYM("120.0000") ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("320.0005")) == get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("370.0007")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //proxied voter bob111111111 decreases stake
   BOOST_REQUIRE_EQUAL( success(), unstake( "bob111111111", STRSYM("50.0001"), STRSYM("50.0001"), STRSYM("100.0002") ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("220.0003")) == get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("270.0005")) == get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //proxied voter carol1111111 chooses another proxy
   BOOST_REQUIRE_EQUAL( success(), vote( N(carol1111111), vector<account_name>(), "donald111111" ) );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("50.0001")), get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("170.0002")), get_voter_info( "donald111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE( stake2votes(STRSYM("100.0003")), get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_REQUIRE_EQUAL( 0, get_producer_info( "defproducer3" )["total_votes"].as_double() );

   //bob111111111 switches to direct voting and votes for one of the same producers, but not for another one
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), { N(defproducer2) } ) );
   BOOST_TEST_REQUIRE( 0.0 == get_voter_info( "alice1111111" )["proxied_vote_weight"].as_double() );
   BOOST_TEST_REQUIRE(  stake2votes(STRSYM("50.0002")), get_producer_info( "defproducer1" )["total_votes"].as_double() );
   BOOST_TEST_REQUIRE( 0.0 == get_producer_info( "defproducer3" )["total_votes"].as_double() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( vote_both_proxy_and_producers, eosio_system_tester ) try {
   //alice1111111 becomes a proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy", true)
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" ), get_voter_info( "alice1111111" ) );

   //carol1111111 becomes a producer
   BOOST_REQUIRE_EQUAL( success(), regproducer( "carol1111111" ) );

   //bob111111111 chooses alice1111111 as a proxy

   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("cannot vote for producers and proxy at same time"),
                        vote( N(bob111111111), { N(carol1111111) }, "alice1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( select_invalid_proxy, eosio_system_tester ) try {
   //accumulate proxied votes
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("10.0000") ) );

   //selecting account not registered as a proxy
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "invalid proxy specified" ),
                        vote( N(bob111111111), vector<account_name>(), "alice1111111" ) );

   //selecting not existing account as a proxy
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "invalid proxy specified" ),
                        vote( N(bob111111111), vector<account_name>(), "notexist" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( double_register_unregister_proxy_keeps_votes, eosio_system_tester ) try {
   //alice1111111 becomes a proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy",  1)
                        )
   );
   issue_and_transfer( "alice1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("5.0000"), STRSYM("5.0000"), STRSYM("10.0000") ) );
   edump((get_voter_info("alice1111111")));
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "staked", 100000 ), get_voter_info( "alice1111111" ) );

   //bob111111111 stakes and selects alice1111111 as a proxy
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("150.0003") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), vector<account_name>(), "alice1111111" ) );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "proxied_vote_weight", stake2votes( STRSYM("150.0003") ))( "staked", 100000 ), get_voter_info( "alice1111111" ) );

   //double regestering should fail without affecting total votes and stake
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "action has no effect" ),
                        push_action( N(alice1111111), N(regproxy), mvo()
                                     ("proxy",  "alice1111111")
                                     ("isproxy",  1)
                        )
   );
   REQUIRE_MATCHING_OBJECT( proxy( "alice1111111" )( "proxied_vote_weight", stake2votes(STRSYM("150.0003")) )( "staked", 100000 ), get_voter_info( "alice1111111" ) );

   //uregister
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy",  0)
                        )
   );
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111" )( "proxied_vote_weight", stake2votes(STRSYM("150.0003")) )( "staked", 100000 ), get_voter_info( "alice1111111" ) );

   //double unregistering should not affect proxied_votes and stake
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "action has no effect" ),
                        push_action( N(alice1111111), N(regproxy), mvo()
                                     ("proxy",  "alice1111111")
                                     ("isproxy",  0)
                        )
   );
   REQUIRE_MATCHING_OBJECT( voter( "alice1111111" )( "proxied_vote_weight", stake2votes(STRSYM("150.0003")))( "staked", 100000 ), get_voter_info( "alice1111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( proxy_cannot_use_another_proxy, eosio_system_tester ) try {
   //alice1111111 becomes a proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(alice1111111), N(regproxy), mvo()
                                                ("proxy",  "alice1111111")
                                                ("isproxy",  1)
                        )
   );

   //bob111111111 becomes a proxy
   BOOST_REQUIRE_EQUAL( success(), push_action( N(bob111111111), N(regproxy), mvo()
                                                ("proxy",  "bob111111111")
                                                ("isproxy",  1)
                        )
   );

   //proxy should not be able to use a proxy
   issue_and_transfer( "bob111111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "account registered as a proxy is not allowed to use a proxy" ),
                        vote( N(bob111111111), vector<account_name>(), "alice1111111" ) );

   //voter that uses a proxy should not be allowed to become a proxy
   issue_and_transfer( "carol1111111", STRSYM("1000.0000"),  config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), stake( "carol1111111", STRSYM("100.0002"), STRSYM("50.0001"), STRSYM("10.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(carol1111111), vector<account_name>(), "alice1111111" ) );
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "account that uses a proxy is not allowed to become a proxy" ),
                        push_action( N(carol1111111), N(regproxy), mvo()
                                     ("proxy",  "carol1111111")
                                     ("isproxy",  1)
                        )
   );

   //proxy should not be able to use itself as a proxy
   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "cannot proxy to self" ),
                        vote( N(bob111111111), vector<account_name>(), "bob111111111" ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( elect_producers /*_and_parameters*/, eosio_system_tester ) try {

   create_accounts_with_resources( {  N(defproducer1), N(defproducer2), N(defproducer3) } );

   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer1" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer2" ) );
   BOOST_REQUIRE_EQUAL( success(), regproducer( "defproducer3" ) );

   const asset min_prod_stake = core_sym::from_int(min_producer_activated_stake);
   const asset half_min_prod_stake = core_sym::from_int(min_producer_activated_stake/2);

   //stake more than 15% of total supply to activate chain
   transfer( "eosio", "alice1111111", STRSYM("30000200.0000") + min_prod_stake, "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("30000000.0000") ) );
   // stake for defproducer1 to reach min_producer_activated_stake
   BOOST_REQUIRE_EQUAL( success(), stake( "alice1111111", "defproducer1", half_min_prod_stake, half_min_prod_stake, STRSYM("0.0000") ) );
   //vote for producers
   BOOST_REQUIRE_EQUAL( success(), vote( N(alice1111111), { N(defproducer1) } ) );
   produce_blocks(250);

   auto producer_keys = control->head_block_state()->active_schedule.producers;
   BOOST_REQUIRE_EQUAL( 1, producer_keys.size() );
   BOOST_REQUIRE_EQUAL( name("defproducer1"), producer_keys[0].producer_name );

   //auto config = config_to_variant( control->get_global_properties().configuration );
   //auto prod1_config = testing::filter_fields( config, producer_parameters_example( 1 ) );
   //REQUIRE_EQUAL_OBJECTS(prod1_config, config);

   // elect 2 producers
   issue_and_transfer( "bob111111111", STRSYM("80200.0000") + min_prod_stake, config::system_account_name );
   ilog("stake");
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("80000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), stake( "bob111111111", "defproducer2", half_min_prod_stake, half_min_prod_stake, STRSYM("0.0000") ) );
   ilog("start vote");
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob111111111), { N(defproducer2) } ) );
   ilog(".");
   produce_blocks(250);
   producer_keys = control->head_block_state()->active_schedule.producers;
   BOOST_REQUIRE_EQUAL( 2, producer_keys.size() );
   BOOST_REQUIRE_EQUAL( name("defproducer1"), producer_keys[0].producer_name );
   BOOST_REQUIRE_EQUAL( name("defproducer2"), producer_keys[1].producer_name );
   //config = config_to_variant( control->get_global_properties().configuration );
   //auto prod2_config = testing::filter_fields( config, producer_parameters_example( 2 ) );
   //REQUIRE_EQUAL_OBJECTS(prod2_config, config);

   // elect 3 producers
   issue_and_transfer( N(defproducer3), STRSYM("80200.0000"), config::system_account_name );
   // total defproducer3 stake > min_producer_activated_stake
   BOOST_REQUIRE_EQUAL( success(), stake( N(defproducer3), STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("80000.0000") ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(defproducer3), { N(defproducer3) } ) );
   produce_blocks(250);
   producer_keys = control->head_block_state()->active_schedule.producers;
   BOOST_REQUIRE_EQUAL( 3, producer_keys.size() );
   BOOST_REQUIRE_EQUAL( name("defproducer1"), producer_keys[0].producer_name );
   BOOST_REQUIRE_EQUAL( name("defproducer2"), producer_keys[1].producer_name );
   BOOST_REQUIRE_EQUAL( name("defproducer3"), producer_keys[2].producer_name );
   //config = config_to_variant( control->get_global_properties().configuration );
   //REQUIRE_EQUAL_OBJECTS(prod2_config, config);

   wdump((get_producer_info("defproducer1")));
   wdump((get_producer_info("defproducer2")));
   wdump((get_producer_info("defproducer3")));

   // retract votes for defproducer3 => decrease schedule size back to 2 producers
   BOOST_REQUIRE_EQUAL( success(), vote( N(defproducer3), { } ) );
   produce_blocks(250);

   wdump((get_producer_info("defproducer1")));
   wdump((get_producer_info("defproducer2")));
   wdump((get_producer_info("defproducer3")));
   //~print_debug_logs();
   BOOST_TEST_MESSAGE("gstate.last_producer_schedule_size = " << get_global_state()["last_producer_schedule_size"].as<uint16_t>());

   producer_keys = control->head_block_state()->active_schedule.producers;
   BOOST_REQUIRE_EQUAL( 2, producer_keys.size() );

   // The test below is invalid now, producer schedule is not updated if there are
   // fewer producers in the new schedule
   /*
   BOOST_REQUIRE_EQUAL( 2, producer_keys.size() );
   BOOST_REQUIRE_EQUAL( name("defproducer1"), producer_keys[0].producer_name );
   BOOST_REQUIRE_EQUAL( name("defproducer3"), producer_keys[1].producer_name );
   //config = config_to_variant( control->get_global_properties().configuration );
   //auto prod3_config = testing::filter_fields( config, producer_parameters_example( 3 ) );
   //REQUIRE_EQUAL_OBJECTS(prod3_config, config);
   */

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( buyname, eosio_system_tester ) try {
   create_accounts_with_resources( { N(dan), N(sam) } );
   transfer( config::system_account_name, "dan", STRSYM( "10000.0000" ) );
   transfer( config::system_account_name, "sam", STRSYM( "10000.0000" ) );
   stake_with_transfer( config::system_account_name, "sam", STRSYM("1000.0000"), STRSYM("1000.0000"), STRSYM("30000000.0000") );
   stake_with_transfer( config::system_account_name, "dan", STRSYM("1000.0000"), STRSYM("1000.0000"), STRSYM("30000000.0000") );

   regproducer( config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), vote( N(sam), { config::system_account_name } ) );
   // wait 14 days after min required amount has been staked
   produce_block(fc::days(7));
   BOOST_REQUIRE_EQUAL( success(), vote(N(dan), { config::system_account_name }) );
   produce_block(fc::days(7));
   produce_block();

   BOOST_REQUIRE_EXCEPTION( create_accounts_with_resources({ N(fail) }, N(dan)), // dan shouldn't be able to create fail
                            eosio_assert_message_exception, eosio_assert_message_is("no active bid for name") );
   const auto log = bidname("dan", "nofail", STRSYM("1.0000"));
   BOOST_TEST_MESSAGE( "bidname(): " << log );
   debug_name_bids({ N(eosio), N(sam), N(dan), N(nofail) });

   // didn't increase bid by 10%
   BOOST_REQUIRE_EQUAL( "assertion failure with message: must increase bid by 10%", bidname("sam", "nofail", STRSYM("1.0000")) );
   debug_name_bids({ N(eosio), N(sam), N(dan), N(nofail) });

   BOOST_REQUIRE_EQUAL( success(), bidname("sam", "nofail", STRSYM("2.0000")) );
   debug_name_bids({ N(eosio), N(sam), N(dan), N(nofail) });

   produce_block(fc::days(1));
   produce_block();
   BOOST_TEST_MESSAGE("After 1 day + 1 block auction should be closed for nofail:");
   debug_name_bids({ N(eosio), N(sam), N(dan), N(nofail) });
   print_debug_logs();

   BOOST_REQUIRE_LT(get_name_bid(N(nofail))["high_bid"].as<int64_t>(), 0);

   BOOST_REQUIRE_EXCEPTION( create_account_with_resources(N(nofail), N(dan)), // dan shoudn't be able to do this, sam won
                            eosio_assert_message_exception, eosio_assert_message_is("only highest bidder can claim") );
   BOOST_TEST_MESSAGE("verify sam can create nofail");
   debug_balances({N(sam), N(dan), N(nofail)});

   create_account_with_resources(N(nofail), N(sam)); // sam should be able to do this, he won the bid

   BOOST_TEST_MESSAGE("verify nofail can create test.nofail");
   transfer("eosio", "nofail", STRSYM("1000.0000"));
   create_account_with_resources( N(test.nofail), N(nofail) ); // only nofail can create test.nofail
   BOOST_TEST_MESSAGE("verify dan cannot create test.fail");
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources(N(test.fail), N(dan)), // dan shouldn't be able to do this
                            eosio_assert_message_exception, eosio_assert_message_is("only suffix may create this account") );

   create_account_with_resources(N(goodgoodgood), N(dan)); /// 12 char names should succeed
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( bid_invalid_names, eosio_system_tester ) try {
   create_accounts_with_resources( { N(dan) } );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "you can only bid on top-level suffix" ),
                        bidname( "dan", "abcdefg.12345", STRSYM( "1.0000" ) ) );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "the empty name is not a valid account name to bid on" ),
                        bidname( "dan", "", STRSYM( "1.0000" ) ) );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "13 character names are not valid account names to bid on" ),
                        bidname( "dan", "abcdefgh12345", STRSYM( "1.0000" ) ) );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "accounts with 12 character names and no dots can be created without bidding required" ),
                        bidname( "dan", "abcdefg12345", STRSYM( "1.0000" ) ) );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( multiple_namebids, eosio_system_tester ) try {
   const std::string not_closed_message("auction for name is not closed yet");

   std::vector<account_name> accounts = { N(alice), N(bob), N(carl), N(david), N(eve) };
   create_accounts_with_resources( accounts );
   for ( const auto& a: accounts ) {
      transfer( config::system_account_name, a, STRSYM( "10000.0000" ) );
      BOOST_REQUIRE_EQUAL( STRSYM( "10000.0000" ), get_balance(a) );
   }
   create_accounts_with_resources( { N(producer) } );
   BOOST_REQUIRE_EQUAL( success(), regproducer( N(producer) ) );

   produce_block();
   // stake but but not enough to go live
   stake_with_transfer( config::system_account_name, "bob", STRSYM("35.0000"), STRSYM("35.0000"), STRSYM("3000000.0000") );
   stake_with_transfer( config::system_account_name, "carl", STRSYM("35.0000"), STRSYM("35.0000"), STRSYM("3000000.0000") );
   BOOST_REQUIRE_EQUAL( success(), vote( N(bob), { N(producer) } ) );
   BOOST_REQUIRE_EQUAL( success(), vote( N(carl), { N(producer) } ) );

   // start bids
   bidname( "bob",  "prefa", STRSYM("1.0003") );
   BOOST_REQUIRE_EQUAL( STRSYM( "9998.9997" ), get_balance("bob") );
   bidname( "bob",  "prefb", STRSYM("1.0000") );
   bidname( "bob",  "prefc", STRSYM("1.0000") );
   BOOST_REQUIRE_EQUAL( STRSYM( "9996.9997" ), get_balance("bob") );

   bidname( "carl", "prefd", STRSYM("1.0000") );
   bidname( "carl", "prefe", STRSYM("1.0000") );
   BOOST_REQUIRE_EQUAL( STRSYM( "9998.0000" ), get_balance("carl") );

   BOOST_REQUIRE_EQUAL( error("assertion failure with message: account is already highest bidder"),
                        bidname( "bob", "prefb", STRSYM("1.1001") ) );
   BOOST_REQUIRE_EQUAL( error("assertion failure with message: must increase bid by 10%"),
                        bidname( "alice", "prefb", STRSYM("1.0999") ) );
   BOOST_REQUIRE_EQUAL( STRSYM( "9996.9997" ), get_balance("bob") );
   BOOST_REQUIRE_EQUAL( STRSYM( "10000.0000" ), get_balance("alice") );

   // alice outbids bob on prefb
   {
      const asset initial_names_balance = get_balance(N(eosio.names));
      BOOST_REQUIRE_EQUAL( success(),
                           bidname( "alice", "prefb", STRSYM("1.1001") ) );
      BOOST_REQUIRE_EQUAL( STRSYM( "9997.9997" ), get_balance("bob") );
      BOOST_REQUIRE_EQUAL( STRSYM( "9998.8999" ), get_balance("alice") );
      BOOST_REQUIRE_EQUAL( initial_names_balance + STRSYM("0.1001"), get_balance(N(eosio.names)) );
   }

   // david outbids carl on prefd
   {
      BOOST_REQUIRE_EQUAL( STRSYM( "9998.0000" ), get_balance("carl") );
      BOOST_REQUIRE_EQUAL( STRSYM( "10000.0000" ), get_balance("david") );
      BOOST_REQUIRE_EQUAL( success(),
                           bidname( "david", "prefd", STRSYM("1.9900") ) );
      BOOST_REQUIRE_EQUAL( STRSYM( "9999.0000" ), get_balance("carl") );
      BOOST_REQUIRE_EQUAL( STRSYM( "9998.0100" ), get_balance("david") );
   }

   // eve outbids carl on prefe
   {
      BOOST_REQUIRE_EQUAL( success(),
                           bidname( "eve", "prefe", STRSYM("1.7200") ) );
   }

   produce_block( fc::days(14) );
   produce_block();

   // highest bid is from david for prefd but no bids can be closed yet
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefd), N(david) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );

   // stake enough to go above the 15% threshold
   stake_with_transfer( config::system_account_name, "alice", STRSYM( "10.0000" ), STRSYM( "10.0000" ), STRSYM("20000000.0000") );
   BOOST_REQUIRE_EQUAL(0, get_producer_info("producer")["unpaid_blocks"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL( success(), vote( N(alice), { N(producer) } ) );

   // need to wait for 14 days after going live
   produce_blocks(10);
   produce_block( fc::days(2) );
   produce_blocks( 10 );
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefd), N(david) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   // it's been 14 days, auction for prefd has been closed
   produce_block( fc::days(12) );
   create_account_with_resources( N(prefd), N(david) );
   produce_blocks(2);
   produce_block( fc::hours(23) );
   // auctions for prefa, prefb, prefc, prefe haven't been closed
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefa), N(bob) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefb), N(alice) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefc), N(bob) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefe), N(eve) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   // attemp to create account with no bid
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefg), N(alice) ),
                            fc::exception, fc_assert_exception_message_is( "no active bid for name" ) );
   // changing highest bid pushes auction closing time by 24 hours
   BOOST_REQUIRE_EQUAL( success(),
                        bidname( "eve",  "prefb", STRSYM("2.1880") ) );

   produce_block( fc::hours(22) );
   produce_blocks(2);

   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefb), N(eve) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   // but changing a bid that is not the highest does not push closing time
   BOOST_REQUIRE_EQUAL( success(),
                        bidname( "carl", "prefe", STRSYM("2.0980") ) );
   produce_block( fc::hours(2) );
   produce_blocks(2);
   // bid for prefb has closed, only highest bidder can claim
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefb), N(alice) ),
                            eosio_assert_message_exception, eosio_assert_message_is( "only highest bidder can claim" ) );
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefb), N(carl) ),
                            eosio_assert_message_exception, eosio_assert_message_is( "only highest bidder can claim" ) );
   create_account_with_resources( N(prefb), N(eve) );

   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefe), N(carl) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );
   produce_block();
   produce_block( fc::hours(24) );
   // by now bid for prefe has closed
   create_account_with_resources( N(prefe), N(carl) );
   // prefe can now create *.prefe
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(xyz.prefe), N(carl) ),
                            fc::exception, fc_assert_exception_message_is("only suffix may create this account") );
   transfer( config::system_account_name, N(prefe), STRSYM("10000.0000") );
   create_account_with_resources( N(xyz.prefe), N(prefe) );

   // other auctions haven't closed
   BOOST_REQUIRE_EXCEPTION( create_account_with_resources( N(prefa), N(bob) ),
                            fc::exception, fc_assert_exception_message_is( not_closed_message ) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( namebid_pending_winner, eosio_system_tester ) try {
   cross_15_percent_threshold();
   produce_block( fc::hours(14*24) );    //wait 14 day for name auction activation
   transfer( config::system_account_name, N(alice1111111), STRSYM("10000.0000") );
   transfer( config::system_account_name, N(bob111111111), STRSYM("10000.0000") );

   BOOST_REQUIRE_EQUAL( success(), bidname( "alice1111111", "prefa", STRSYM( "50.0000" ) ));
   BOOST_REQUIRE_EQUAL( success(), bidname( "bob111111111", "prefb", STRSYM( "30.0000" ) ));
   produce_block( fc::hours(100) ); //should close "perfa"
   produce_block( fc::hours(100) ); //should close "perfb"

   //despite "perfa" account hasn't been created, we should be able to create "perfb" account
   create_account_with_resources( N(prefb), N(bob111111111) );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( vote_producers_in_and_out, eosio_system_tester ) try {

   const asset net = STRSYM("80.0000");
   const asset cpu = STRSYM("80.0000");
   asset vote_stake = STRSYM("3000000.0000");

   // create accounts {defproducera, defproducerb, ..., defproducerz},
   // {producvotera, producvoterb, ..., producvoterz} and register as producers
   std::vector<account_name> producer_names;
   std::vector<account_name> voters;
   {
      producer_names.reserve('z' - 'a' + 1);
      voters.reserve('z' - 'a' + 1);
      const std::string prod_root("defproducer");
      const std::string voter_root("producvoter");
      for ( char c = 'a'; c <= 'z'; ++c ) {
         producer_names.emplace_back(prod_root + std::string(1, c));
         voters.emplace_back(voter_root + std::string(1, c));
      }
      setup_producer_accounts(producer_names);
      for (const auto& p: producer_names) {
         BOOST_REQUIRE_EQUAL( success(), regproducer(p) );
         transfer( config::system_account_name, p, STRSYM("5.0000"), config::system_account_name );
         BOOST_REQUIRE_EQUAL( success(), stake(p, STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("1.0000")) );
         produce_blocks(1);
         ilog( "------ get pro----------" );
         wdump((p));
         BOOST_TEST(0 == get_producer_info(p)["total_votes"].as<double>());
      }
   }

   for (const auto& v: voters) {
      create_account_with_resources(v, config::system_account_name, STRSYM("1.0000"), false, net, cpu, vote_stake, true);
      vote_stake += STRSYM("1.0000");
   }

   for (auto i = 0; i < 21; ++i) {
      BOOST_REQUIRE_EQUAL(success(), vote(voters[i], std::vector<account_name> { producer_names[i] }));
   }

   // give a chance for everyone to produce blocks
   {
      produce_blocks(5000);
      bool all_21_produced = true;
      for (uint32_t i = 0; i < 21; ++i) {
         if (0 == get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            all_21_produced = false;
            wdump((get_producer_info(producer_names[i])));
         }
      }
      bool rest_didnt_produce = true;
      for (uint32_t i = 21; i < producer_names.size(); ++i) {
         if (0 < get_producer_info(producer_names[i])["unpaid_blocks"].as<uint32_t>()) {
            rest_didnt_produce = false;
            wdump((get_producer_info(producer_names[i])));
         }
      }
      BOOST_REQUIRE(all_21_produced && rest_didnt_produce);
   }

   {
      produce_block(fc::hours(7));
      const uint32_t voted_out_index = 0;
      const uint32_t new_prod_index  = 22;
      BOOST_REQUIRE_EQUAL(success(), vote(voters[new_prod_index], { producer_names[new_prod_index] }));
      BOOST_REQUIRE_EQUAL(0, get_producer_info(producer_names[new_prod_index])["unpaid_blocks"].as<uint32_t>());
      produce_blocks(4 * 12 * 21);
      BOOST_REQUIRE(0 < get_producer_info(producer_names[new_prod_index])["unpaid_blocks"].as<uint32_t>());
      const uint32_t initial_unpaid_blocks = get_producer_info(producer_names[voted_out_index])["unpaid_blocks"].as<uint32_t>();
      produce_blocks(2 * 12 * 21);
      BOOST_REQUIRE_EQUAL(initial_unpaid_blocks, get_producer_info(producer_names[voted_out_index])["unpaid_blocks"].as<uint32_t>());
      produce_block(fc::hours(24));
      BOOST_REQUIRE_EQUAL(success(), vote(voters[new_prod_index], { producer_names[voted_out_index] }));
      produce_blocks(2 * 12 * 21);
      BOOST_REQUIRE(fc::crypto::public_key() != fc::crypto::public_key(get_producer_info(producer_names[voted_out_index])["producer_key"].as_string()));
      BOOST_REQUIRE_EQUAL(success(), push_action(producer_names[voted_out_index], N(claimrewards), mvo()("owner", producer_names[voted_out_index])));
   }

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( setparams, eosio_system_tester ) try {
   //install multisig contract
   abi_serializer msig_abi_ser = initialize_multisig();
   auto producer_names = active_and_vote_producers();

   //helper function
   auto push_action_msig = [&]( const account_name& signer, const action_name &name, const variant_object &data, bool auth = true ) -> action_result {
         string action_type_name = msig_abi_ser.get_action_type(name);

         action act;
         act.account = N(eosio.msig);
         act.name = name;
         act.data = msig_abi_ser.variant_to_binary( action_type_name, data, abi_serializer_max_time );

         return base_tester::push_action( std::move(act), auth ? uint64_t(signer) : signer == N(bob111111111) ? N(alice1111111) : N(bob111111111) );
   };

   // test begins
   vector<permission_level> prod_perms;
   for ( auto& x : producer_names ) {
      prod_perms.push_back( { name(x), config::active_name } );
   }

   eosio::chain::chain_config params;
   params = control->get_global_properties().configuration;
   //change some values
   params.max_block_net_usage += 10;
   params.max_transaction_lifetime += 1;

   transaction trx;
   {
      variant pretty_trx = fc::mutable_variant_object()
         ("expiration", "2020-01-01T00:30")
         ("ref_block_num", 2)
         ("ref_block_prefix", 3)
         ("net_usage_words", 0)
         ("max_cpu_usage_ms", 0)
         ("delay_sec", 0)
         ("actions", fc::variants({
               fc::mutable_variant_object()
                  ("account", name(config::system_account_name))
                  ("name", "setparams")
                  ("authorization", vector<permission_level>{ { config::system_account_name, config::active_name } })
                  ("data", fc::mutable_variant_object()
                   ("params", params)
                  )
                  })
         );
      abi_serializer::from_variant(pretty_trx, trx, get_resolver(), abi_serializer_max_time);
   }

   BOOST_REQUIRE_EQUAL(success(), push_action_msig( N(alice1111111), N(propose), mvo()
                                                    ("proposer",      "alice1111111")
                                                    ("proposal_name", "setparams1")
                                                    ("trx",           trx)
                                                    ("requested", prod_perms)
                       )
   );

   // get 16 approvals
   for ( size_t i = 0; i < 15; ++i ) {
      BOOST_REQUIRE_EQUAL(success(), push_action_msig( name(producer_names[i]), N(approve), mvo()
                                                       ("proposer",      "alice1111111")
                                                       ("proposal_name", "setparams1")
                                                       ("level",         permission_level{ name(producer_names[i]), config::active_name })
                          )
      );
   }

   transaction_trace_ptr trace;
   control->applied_transaction.connect(
   [&]( std::tuple<const transaction_trace_ptr&, const signed_transaction&> p ) {
      const auto& t = std::get<0>(p);
      if( t->scheduled ) { trace = t; }
   } );

   BOOST_REQUIRE_EQUAL(success(), push_action_msig( N(alice1111111), N(exec), mvo()
                                                    ("proposer",      "alice1111111")
                                                    ("proposal_name", "setparams1")
                                                    ("executer",      "alice1111111")
                       )
   );

   BOOST_REQUIRE( bool(trace) );
   BOOST_REQUIRE_EQUAL( 1, trace->action_traces.size() );
   BOOST_REQUIRE_EQUAL( transaction_receipt::executed, trace->receipt->status );

   produce_blocks( 250 );

   // make sure that changed parameters were applied
   auto active_params = control->get_global_properties().configuration;
   BOOST_REQUIRE_EQUAL( params.max_block_net_usage, active_params.max_block_net_usage );
   BOOST_REQUIRE_EQUAL( params.max_transaction_lifetime, active_params.max_transaction_lifetime );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( setram_effect, eosio_system_tester ) try {

   const asset net = STRSYM("8.0000");
   const asset cpu = STRSYM("8.0000");
   std::vector<account_name> accounts = { N(aliceaccount), N(bobbyaccount) };
   for (const auto& a: accounts) {
      create_account_with_resources(a, config::system_account_name, STRSYM("1.0000"), false, net, cpu);
   }

   {
      const auto name_a = accounts[0];
      transfer( config::system_account_name, name_a, STRSYM("1000.0000") );
      BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance(name_a) );
      const uint64_t init_bytes_a = get_total_stake(name_a)["ram_bytes"].as_uint64();
      BOOST_REQUIRE_EQUAL( success(), buyram( name_a, name_a, STRSYM("300.0000") ) );
      BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance(name_a) );
      const uint64_t bought_bytes_a = get_total_stake(name_a)["ram_bytes"].as_uint64() - init_bytes_a;

      // after buying and selling balance should be 700 + 300 * 0.995 * 0.995 = 997.0075 (actually 997.0074 due to rounding fees up)
      BOOST_REQUIRE_EQUAL( success(), sellram(name_a, bought_bytes_a ) );
      BOOST_REQUIRE_EQUAL( STRSYM("997.0074"), get_balance(name_a) );
   }

   {
      const auto name_b = accounts[1];
      transfer( config::system_account_name, name_b, STRSYM("1000.0000") );
      BOOST_REQUIRE_EQUAL( STRSYM("1000.0000"), get_balance(name_b) );
      const uint64_t init_bytes_b = get_total_stake(name_b)["ram_bytes"].as_uint64();
      // name_b buys ram at current price
      BOOST_REQUIRE_EQUAL( success(), buyram( name_b, name_b, STRSYM("300.0000") ) );
      BOOST_REQUIRE_EQUAL( STRSYM("700.0000"), get_balance(name_b) );
      const uint64_t bought_bytes_b = get_total_stake(name_b)["ram_bytes"].as_uint64() - init_bytes_b;

      // increase max_ram_size, ram bought by name_b loses part of its value
      BOOST_REQUIRE_EQUAL( wasm_assert_msg("ram may only be increased"),
                           push_action(config::system_account_name, N(setram), mvo()("max_ram_size", 64ll*1024 * 1024 * 1024)) );
      BOOST_REQUIRE_EQUAL( error("missing authority of eosio"),
                           push_action(name_b, N(setram), mvo()("max_ram_size", 80ll*1024 * 1024 * 1024)) );
      BOOST_REQUIRE_EQUAL( success(),
                           push_action(config::system_account_name, N(setram), mvo()("max_ram_size", 80ll*1024 * 1024 * 1024)) );

      BOOST_REQUIRE_EQUAL( success(), sellram(name_b, bought_bytes_b ) );
      BOOST_REQUIRE( STRSYM("900.0000") < get_balance(name_b) );
      BOOST_REQUIRE( STRSYM("950.0000") > get_balance(name_b) );
   }

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( ram_inflation, eosio_system_tester ) try {

   const uint64_t init_max_ram_size = 64ll*1024 * 1024 * 1024;

   BOOST_REQUIRE_EQUAL( init_max_ram_size, get_global_state()["max_ram_size"].as_uint64() );
   produce_blocks(20);
   BOOST_REQUIRE_EQUAL( init_max_ram_size, get_global_state()["max_ram_size"].as_uint64() );
   transfer( config::system_account_name, "alice1111111", STRSYM("1000.0000"), config::system_account_name );
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   produce_blocks(3);
   BOOST_REQUIRE_EQUAL( init_max_ram_size, get_global_state()["max_ram_size"].as_uint64() );
   uint16_t rate = 1000;
   BOOST_REQUIRE_EQUAL( success(), push_action( config::system_account_name, N(setramrate), mvo()("bytes_per_block", rate) ) );
   BOOST_REQUIRE_EQUAL( rate, get_global_state2()["new_ram_per_block"].as<uint16_t>() );
   // last time update_ram_supply called is in buyram, num of blocks since then to
   // the block that includes the setramrate action is 1 + 3 = 4.
   // However, those 4 blocks were accumulating at a rate of 0, so the max_ram_size should not have changed.
   BOOST_REQUIRE_EQUAL( init_max_ram_size, get_global_state()["max_ram_size"].as_uint64() );
   // But with additional blocks, it should start accumulating at the new rate.
   uint64_t cur_ram_size = get_global_state()["max_ram_size"].as_uint64();
   produce_blocks(10);
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("100.0000") ) );
   BOOST_REQUIRE_EQUAL( cur_ram_size + 11 * rate, get_global_state()["max_ram_size"].as_uint64() );
   cur_ram_size = get_global_state()["max_ram_size"].as_uint64();
   produce_blocks(5);
   BOOST_REQUIRE_EQUAL( cur_ram_size, get_global_state()["max_ram_size"].as_uint64() );
   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", 100 ) );
   BOOST_REQUIRE_EQUAL( cur_ram_size + 6 * rate, get_global_state()["max_ram_size"].as_uint64() );
   cur_ram_size = get_global_state()["max_ram_size"].as_uint64();
   produce_blocks();
   BOOST_REQUIRE_EQUAL( success(), buyrambytes( "alice1111111", "alice1111111", 100 ) );
   BOOST_REQUIRE_EQUAL( cur_ram_size + 2 * rate, get_global_state()["max_ram_size"].as_uint64() );

   BOOST_REQUIRE_EQUAL( error("missing authority of eosio"),
                        push_action( "alice1111111", N(setramrate), mvo()("bytes_per_block", rate) ) );

   cur_ram_size = get_global_state()["max_ram_size"].as_uint64();
   produce_blocks(10);
   uint16_t old_rate = rate;
   rate = 5000;
   BOOST_REQUIRE_EQUAL( success(), push_action( config::system_account_name, N(setramrate), mvo()("bytes_per_block", rate) ) );
   BOOST_REQUIRE_EQUAL( cur_ram_size + 11 * old_rate, get_global_state()["max_ram_size"].as_uint64() );
   produce_blocks(5);
   BOOST_REQUIRE_EQUAL( success(), buyrambytes( "alice1111111", "alice1111111", 100 ) );
   BOOST_REQUIRE_EQUAL( cur_ram_size + 11 * old_rate + 6 * rate, get_global_state()["max_ram_size"].as_uint64() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( eosioram_ramusage, eosio_system_tester ) try {
   BOOST_REQUIRE_EQUAL( STRSYM("0.0000"), get_balance( "alice1111111" ) );
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "eosio", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );

   const asset initial_ram_balance = get_balance(N(eosio.ram));
   const asset initial_ramfee_balance = get_balance(N(eosio.ramfee));
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("1000.0000") ) );

   BOOST_REQUIRE_EQUAL( false, get_row_by_account( N(eosio.token), N(alice1111111), N(accounts), symbol{CORE_SYM}.to_symbol_code() ).empty() );

   //remove row
   base_tester::push_action( N(eosio.token), N(close), N(alice1111111), mvo()
                             ( "owner", "alice1111111" )
                             ( "symbol", symbol{CORE_SYM} )
   );
   BOOST_REQUIRE_EQUAL( true, get_row_by_account( N(eosio.token), N(alice1111111), N(accounts), symbol{CORE_SYM}.to_symbol_code() ).empty() );

   auto rlm = control->get_resource_limits_manager();
   auto eosioram_ram_usage = rlm.get_account_ram_usage(N(eosio.ram));
   auto alice_ram_usage = rlm.get_account_ram_usage(N(alice1111111));

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", 2048 ) );

   //make sure that ram was billed to alice, not to eosio.ram
   BOOST_REQUIRE_EQUAL( true, alice_ram_usage < rlm.get_account_ram_usage(N(alice1111111)) );
   BOOST_REQUIRE_EQUAL( eosioram_ram_usage, rlm.get_account_ram_usage(N(eosio.ram)) );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( ram_gift, eosio_system_tester ) try {
   active_and_vote_producers();

   auto rlm = control->get_resource_limits_manager();
   int64_t ram_bytes_orig, net_weight, cpu_weight;
   rlm.get_account_limits( N(alice1111111), ram_bytes_orig, net_weight, cpu_weight );

   /*
    * It seems impossible to write this test, because buyrambytes action doesn't give you exact amount of bytes requested
    *
   //check that it's possible to create account bying required_bytes(2724) + userres table(112) + userres row(160) - ram_gift_bytes(1400)
   create_account_with_resources( N(abcdefghklmn), N(alice1111111), 2724 + 112 + 160 - 1400 );

   //check that one byte less is not enough
   BOOST_REQUIRE_THROW( create_account_with_resources( N(abcdefghklmn), N(alice1111111), 2724 + 112 + 160 - 1400 - 1 ),
                        ram_usage_exceeded );
   */

   //check that stake/unstake keeps the gift
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(), stake( "eosio", "alice1111111", STRSYM("200.0000"), STRSYM("100.0000"), STRSYM("0.0000") ) );
   int64_t ram_bytes_after_stake;
   rlm.get_account_limits( N(alice1111111), ram_bytes_after_stake, net_weight, cpu_weight );
   BOOST_REQUIRE_EQUAL( ram_bytes_orig, ram_bytes_after_stake );

   BOOST_REQUIRE_EQUAL( success(), unstake( "eosio", "alice1111111", STRSYM("20.0000"), STRSYM("10.0000"), STRSYM("0.0000") ) );
   int64_t ram_bytes_after_unstake;
   rlm.get_account_limits( N(alice1111111), ram_bytes_after_unstake, net_weight, cpu_weight );
   BOOST_REQUIRE_EQUAL( ram_bytes_orig, ram_bytes_after_unstake );

   uint64_t ram_gift = 1400;

   int64_t ram_bytes;
   BOOST_REQUIRE_EQUAL( success(), buyram( "alice1111111", "alice1111111", STRSYM("1000.0000") ) );
   rlm.get_account_limits( N(alice1111111), ram_bytes, net_weight, cpu_weight );
   auto userres = get_total_stake( N(alice1111111) );
   BOOST_REQUIRE_EQUAL( userres["ram_bytes"].as_uint64() + ram_gift, ram_bytes );

   BOOST_REQUIRE_EQUAL( success(), sellram( "alice1111111", 1024 ) );
   rlm.get_account_limits( N(alice1111111), ram_bytes, net_weight, cpu_weight );
   userres = get_total_stake( N(alice1111111) );
   BOOST_REQUIRE_EQUAL( userres["ram_bytes"].as_uint64() + ram_gift, ram_bytes );

} FC_LOG_AND_RETHROW()


BOOST_AUTO_TEST_CASE( setabi_bios ) try {
   validating_tester t( validating_tester::default_config() );
   t.execute_setup_policy( setup_policy::preactivate_feature_only );
   abi_serializer abi_ser(fc::json::from_string( (const char*)contracts::bios_abi().data()).template as<abi_def>(), base_tester::abi_serializer_max_time);
   t.set_code( config::system_account_name, contracts::bios_wasm() );
   t.set_abi( config::system_account_name, contracts::bios_abi().data() );
   t.create_account(N(eosio.token));
   t.set_abi( N(eosio.token), contracts::token_abi().data() );
   {
      auto res = t.get_row_by_account( config::system_account_name, config::system_account_name, N(abihash), N(eosio.token) );
      _abi_hash abi_hash;
      auto abi_hash_var = abi_ser.binary_to_variant( "abi_hash", res, base_tester::abi_serializer_max_time );
      abi_serializer::from_variant( abi_hash_var, abi_hash, t.get_resolver(), base_tester::abi_serializer_max_time);
      auto abi = fc::raw::pack(fc::json::from_string( (const char*)contracts::token_abi().data()).template as<abi_def>());
      auto result = fc::sha256::hash( (const char*)abi.data(), abi.size() );

      BOOST_REQUIRE( abi_hash.hash == result );
   }

   t.set_abi( N(eosio.token), contracts::system_abi().data() );
   {
      auto res = t.get_row_by_account( config::system_account_name, config::system_account_name, N(abihash), N(eosio.token) );
      _abi_hash abi_hash;
      auto abi_hash_var = abi_ser.binary_to_variant( "abi_hash", res, base_tester::abi_serializer_max_time );
      abi_serializer::from_variant( abi_hash_var, abi_hash, t.get_resolver(), base_tester::abi_serializer_max_time);
      auto abi = fc::raw::pack(fc::json::from_string( (const char*)contracts::system_abi().data()).template as<abi_def>());
      auto result = fc::sha256::hash( (const char*)abi.data(), abi.size() );

      BOOST_REQUIRE( abi_hash.hash == result );
   }
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( setabi, eosio_system_tester ) try {
   set_abi( N(eosio.token), contracts::token_abi().data() );
   {
      auto res = get_row_by_account( config::system_account_name, config::system_account_name, N(abihash), N(eosio.token) );
      _abi_hash abi_hash;
      auto abi_hash_var = abi_ser.binary_to_variant( "abi_hash", res, abi_serializer_max_time );
      abi_serializer::from_variant( abi_hash_var, abi_hash, get_resolver(), abi_serializer_max_time);
      auto abi = fc::raw::pack(fc::json::from_string( (const char*)contracts::token_abi().data()).template as<abi_def>());
      auto result = fc::sha256::hash( (const char*)abi.data(), abi.size() );

      BOOST_REQUIRE( abi_hash.hash == result );
   }

   set_abi( N(eosio.token), contracts::system_abi().data() );
   {
      auto res = get_row_by_account( config::system_account_name, config::system_account_name, N(abihash), N(eosio.token) );
      _abi_hash abi_hash;
      auto abi_hash_var = abi_ser.binary_to_variant( "abi_hash", res, abi_serializer_max_time );
      abi_serializer::from_variant( abi_hash_var, abi_hash, get_resolver(), abi_serializer_max_time);
      auto abi = fc::raw::pack(fc::json::from_string( (const char*)contracts::system_abi().data()).template as<abi_def>());
      auto result = fc::sha256::hash( (const char*)abi.data(), abi.size() );

      BOOST_REQUIRE( abi_hash.hash == result );
   }

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( change_limited_account_back_to_unlimited, eosio_system_tester ) try {
   BOOST_REQUIRE( get_total_stake( "eosio" ).is_null() );

   transfer( N(eosio), N(alice1111111), STRSYM("1.0000") );

   auto error_msg = stake( N(alice1111111), N(eosio), STRSYM("0.0000"), STRSYM("1.0000"), STRSYM("0.0000") );
   auto semicolon_pos = error_msg.find(';');

   BOOST_REQUIRE_EQUAL( error("account eosio has insufficient ram"),
                        error_msg.substr(0, semicolon_pos) );

   int64_t ram_bytes_needed = 0;
   {
      std::istringstream s( error_msg );
      s.seekg( semicolon_pos + 7, std::ios_base::beg );
      s >> ram_bytes_needed;
      ram_bytes_needed += 256; // enough room to cover total_resources_table
   }

   push_action( N(eosio), N(setalimits), mvo()
                                          ("account", "eosio")
                                          ("ram_bytes", ram_bytes_needed)
                                          ("net_weight", -1)
                                          ("cpu_weight", -1)
              );

   stake( N(alice1111111), N(eosio), STRSYM("0.0000"), STRSYM("1.0000"), STRSYM("0.0000") );

   REQUIRE_MATCHING_OBJECT( get_total_stake( "eosio" ), mvo()
      ("owner", "eosio")
      ("net_weight", STRSYM("0.0000"))
      ("cpu_weight", STRSYM("1.0000"))
      ("vote_weight", STRSYM("0.0000"))
      ("ram_bytes",  0)
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "only supports unlimited accounts" ),
                        push_action( N(eosio), N(setalimits), mvo()
                                          ("account", "eosio")
                                          ("ram_bytes", ram_bytes_needed)
                                          ("net_weight", -1)
                                          ("cpu_weight", -1)
                        )
   );

   BOOST_REQUIRE_EQUAL( error( "transaction net usage is too high: 128 > 0" ),
                        push_action( N(eosio), N(setalimits), mvo()
                           ("account", "eosio.saving")
                           ("ram_bytes", -1)
                           ("net_weight", -1)
                           ("cpu_weight", -1)
                        )
   );

   BOOST_REQUIRE_EQUAL( success(),
                        push_action( N(eosio), N(setacctnet), mvo()
                           ("account", "eosio")
                           ("net_weight", -1)
                        )
   );

   BOOST_REQUIRE_EQUAL( success(),
                        push_action( N(eosio), N(setacctcpu), mvo()
                           ("account", "eosio")
                           ("cpu_weight", -1)
                        )
   );

   BOOST_REQUIRE_EQUAL( success(),
                        push_action( N(eosio), N(setalimits), mvo()
                                          ("account", "eosio.saving")
                                          ("ram_bytes", ram_bytes_needed)
                                          ("net_weight", -1)
                                          ("cpu_weight", -1)
                        )
   );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( buy_pin_sell_ram, eosio_system_tester ) try {
   BOOST_REQUIRE( get_total_stake( "eosio" ).is_null() );

   transfer( N(eosio), N(alice1111111), STRSYM("1020.0000") );

   auto error_msg = stake( N(alice1111111), N(eosio), STRSYM("10.0000"), STRSYM("10.0000"), STRSYM("0.0000") );
   auto semicolon_pos = error_msg.find(';');

   BOOST_REQUIRE_EQUAL( error("account eosio has insufficient ram"),
                        error_msg.substr(0, semicolon_pos) );

   // read first number from "account eosio has insufficient ram; needs 1744459 bytes has 1400 bytes"
   int64_t ram_bytes_needed = 0;
   {
      std::istringstream s( error_msg );
      s.seekg( semicolon_pos + 8, std::ios_base::beg );
      s >> ram_bytes_needed;
      ram_bytes_needed += ram_bytes_needed/10; // enough buffer to make up for buyrambytes estimation errors
      ram_bytes_needed *= 10;
   }
   BOOST_TEST_MESSAGE("RAM bytes needed = " << ram_bytes_needed);

   auto alice_original_balance = get_balance( N(alice1111111) );

   BOOST_REQUIRE_EQUAL( success(), buyrambytes( N(alice1111111), N(eosio), static_cast<uint32_t>(ram_bytes_needed) ) );

   auto alice_new_balance = get_balance( N(alice1111111) );
   auto tokens_paid_for_ram = alice_original_balance - alice_new_balance;

   BOOST_TEST_MESSAGE("alice: original balance = " << alice_original_balance
      << ", new balance = " << alice_new_balance
      << ", tokens paid for ram = " << tokens_paid_for_ram);

   auto total_res = get_total_stake( "eosio" );

   BOOST_TEST_MESSAGE("eosio total ram stake = " << total_res["ram_bytes"].as_int64());

   REQUIRE_MATCHING_OBJECT( total_res, mvo()
      ("owner",       "eosio")
      ("net_weight",  STRSYM("0.0000"))
      ("cpu_weight",  STRSYM("0.0000"))
      ("vote_weight", STRSYM("0.0000"))
      ("ram_bytes",   total_res["ram_bytes"].as_int64() )
   );

   BOOST_REQUIRE_EQUAL( wasm_assert_msg( "only supports unlimited accounts" ),
                        push_action( N(eosio), N(setalimits), mvo()
                                          ("account",    "eosio")
                                          ("ram_bytes",  ram_bytes_needed)
                                          ("net_weight", -1)
                                          ("cpu_weight", -1)
                        )
   );

   BOOST_REQUIRE_EQUAL( success(),
                        push_action( N(eosio), N(setacctram), mvo()
                           ("account", "eosio")
                           ("ram_bytes", total_res["ram_bytes"].as_int64() )
                        )
   );

   auto eosio_original_balance = get_balance( N(eosio) );

   BOOST_REQUIRE_EQUAL( success(), sellram( N(eosio), total_res["ram_bytes"].as_int64() ) );

   auto eosio_new_balance = get_balance( N(eosio) );
   auto tokens_received_by_selling_ram = eosio_new_balance - eosio_original_balance;

   BOOST_TEST_MESSAGE("eosio: original balance = " << eosio_original_balance
      << ", new balance = " << eosio_new_balance
      << ", tokens received for ram = " << tokens_received_by_selling_ram);

   double sell_fee_percentage =
      double(tokens_paid_for_ram.get_amount() - tokens_received_by_selling_ram.get_amount()) / tokens_paid_for_ram.get_amount();

   BOOST_TEST_MESSAGE("sell_fee_percentage = " << sell_fee_percentage);

   BOOST_REQUIRE( sell_fee_percentage < 0.012 ); // including some rounding errors

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( delegate_vote_without_transfer, eosio_system_tester ) try {
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("vote can only be transfered or delegated to yourself"),
      push_action( N(alice1111111), N(delegatebw), mvo()
                  ("from",     "alice1111111")
                  ("receiver", "bob111111111")
                  ("stake_net_quantity", STRSYM("10.0000"))
                  ("stake_cpu_quantity", STRSYM("10.0000"))
                  ("stake_vote_quantity", STRSYM("10.0000"))
                  ("transfer", false )
                  , true
      )
   );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( delegate_vote_with_transfer, eosio_system_tester ) try {
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(),
      push_action( N(alice1111111), N(delegatebw), mvo()
                  ("from",     "alice1111111")
                  ("receiver", "bob111111111")
                  ("stake_net_quantity", STRSYM("10.0000"))
                  ("stake_cpu_quantity", STRSYM("10.0000"))
                  ("stake_vote_quantity", STRSYM("10.0000"))
                  ("transfer", true )
                  , true
      )
   );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( delegate_vote_to_herself, eosio_system_tester ) try {
   transfer( "eosio", "alice1111111", STRSYM("1000.0000"), "eosio" );
   BOOST_REQUIRE_EQUAL( success(),
      push_action( N(alice1111111), N(delegatebw), mvo()
                  ("from",     "alice1111111")
                  ("receiver", "alice1111111")
                  ("stake_net_quantity", STRSYM("10.0000"))
                  ("stake_cpu_quantity", STRSYM("10.0000"))
                  ("stake_vote_quantity", STRSYM("10.0000"))
                  ("transfer", false )
                  , true
      )
   );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_total_vs_active, eosio_system_tester ) try {
   const auto init_vote = cross_15_percent_threshold();

   const auto a1 = N(account1);
   const auto p1 = N(producer1);

   const asset vote_a1_p1_1 = STRSYM("100.0000");
   const asset vote_a1_p1_2 = STRSYM("200.0000");
   const asset unvote_a1_p1 = STRSYM("220.0000");

   create_account_with_resources(a1, config::system_account_name, STRSYM("1.0000"), false, STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("0.0000"), true);
   create_account_with_resources(p1, config::system_account_name, STRSYM("1.0000"), false, STRSYM("10.0000"), STRSYM("10.0000"), STRSYM("0.0000"), true);

   issue_and_transfer(a1, STRSYM("1000.0000"), config::system_account_name);
   issue_and_transfer(p1, STRSYM("1000.0000"), config::system_account_name);

   BOOST_TEST_REQUIRE( 0 == get_global_state()["active_stake"].as<int64_t>() );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );

   regproducer(p1);

   // stake
   BOOST_TEST_REQUIRE( success() == stake(a1, STRSYM("10.0000"), STRSYM("10.0000"), vote_a1_p1_1) );
   BOOST_TEST_REQUIRE( success() == vote(a1, {p1}) );

   // active_stake == total_activated_stake
   BOOST_TEST_REQUIRE( vote_a1_p1_1.get_amount() == get_global_state()["active_stake"].as<int64_t>() );

   print_debug_logs();

   // total_activated_stake should not change after setting thresh_activated_stake_time
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );


   BOOST_TEST_REQUIRE( success() == stake(a1, STRSYM("20.0000"), STRSYM("20.0000"), vote_a1_p1_2) );
   BOOST_TEST_REQUIRE( success() == vote(a1, {p1}) );

   BOOST_TEST_MESSAGE("act: " << get_global_state()["active_stake"].as<int64_t>() << "; total: " << get_global_state()["total_activated_stake"].as<int64_t>());

   BOOST_TEST_REQUIRE( (vote_a1_p1_1 + vote_a1_p1_2).get_amount() == get_global_state()["active_stake"].as<int64_t>() );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );

   // unstake
   BOOST_TEST_REQUIRE( success() == unstake(a1, STRSYM("1.0000"), STRSYM("1.0000"), unvote_a1_p1) );

   BOOST_TEST_REQUIRE( (vote_a1_p1_1 + vote_a1_p1_2 - unvote_a1_p1).get_amount() == get_global_state()["active_stake"].as<int64_t>() );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );
} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_active_revoke, eosio_system_tester ) try {
   cross_15_percent_threshold();

   const auto voter = N(voter);
   const auto producer = N(producer);
   const auto voter_staked = STRSYM("100.0000");

   create_account_with_resources(voter, config::system_account_name, STRSYM("1.0000"), false,
                                 STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("0.0000"), true);
   create_account_with_resources(producer, config::system_account_name, STRSYM("1.0000"), false,
                                 STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("0.0000"), true);
   issue_and_transfer(voter, voter_staked, config::system_account_name);
   regproducer(producer);

   // intial state
   BOOST_TEST_REQUIRE( 0 == get_global_state()["active_stake"].as<int64_t>() );

   // stake
   BOOST_TEST_REQUIRE( success() == stake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( success() == vote(voter, {producer}) );
   BOOST_TEST_REQUIRE( voter_staked.get_amount() == get_global_state()["active_stake"].as<int64_t>() );

   // revoke
   BOOST_TEST_REQUIRE( success() == vote(voter, {}) );
   BOOST_TEST_REQUIRE( 0 == get_global_state()["active_stake"].as<int64_t>());

   // revoke again
   BOOST_TEST_REQUIRE( success() == vote(voter, {}) );
   BOOST_TEST_REQUIRE( 0 == get_global_state()["active_stake"].as<int64_t>() );

   // vote
   BOOST_TEST_REQUIRE( success() == vote(voter, {producer}) );
   BOOST_TEST_REQUIRE( voter_staked.get_amount() == get_global_state()["active_stake"].as<int64_t>() );

   // unstake everything
   BOOST_TEST_REQUIRE( success() == unstake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( 0 == get_global_state()["active_stake"].as<int64_t>());

   // stake again
   BOOST_TEST_REQUIRE( success() == stake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( voter_staked.get_amount() == get_global_state()["active_stake"].as<int64_t>() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( total_activated_stake_fix, eosio_system_tester ) try {
   // check, that total_activated_stake is not unfluenced by unstaking & voting again
   const auto init_vote = cross_15_percent_threshold();

   const auto voter = N(voter);
   const auto producer = N(producer);
   const auto voter_staked = STRSYM("100.0000");

   print_debug_logs();

   create_account_with_resources(voter, config::system_account_name, STRSYM("1.0000"), false,
                                 STRSYM("1.0000"), STRSYM("1.0000"), STRSYM("0.0000"), true);
   create_account_with_resources(producer, config::system_account_name, STRSYM("1.0000"), false,
                                 STRSYM("100.0000"), STRSYM("100.0000"), STRSYM("0.0000"), true);
   issue_and_transfer(voter, voter_staked, config::system_account_name);
   regproducer(producer);

   // initial state
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );

   // stake
   BOOST_TEST_REQUIRE( success() == stake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( success() == vote(voter, {producer}) );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );

   // unstake and stake again
   BOOST_TEST_REQUIRE( success() == unstake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );
   BOOST_TEST_REQUIRE( success() == stake(voter, STRSYM("0.0000"), STRSYM("0.0000"), voter_staked) );
   BOOST_TEST_REQUIRE( init_vote.get_amount() == get_global_state()["total_activated_stake"].as<int64_t>() );

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( stake_validators_correlation, eosio_system_tester ) try {
   BOOST_TEST_MESSAGE("state: " << variant_to_string(get_global_state()));
   //BOOST_TEST_MESSAGE("state: " << get_global_state()); // TODO: make it working

   const size_t nproducers = 50;
   //const unsigned max_active_producers = 102; // maximum number of active producers
   //const unsigned max_schedule_update_delta = 3; // contract allows to add/remove maximum 3 producers per 24 hours

   const auto producers = generate_names(nproducers);
   BOOST_TEST_MESSAGE("producer namess: " << bal::join(producers | bad::transformed([](const auto& e){ return e.to_string(); }), " "));

   const asset vote_stake = core_sym::from_int(get_token_supply().get_amount() / nproducers / 3);
   BOOST_TEST_MESSAGE("vote stake used for each BP: " << vote_stake.to_string());

   size_t registered_prods = 0;

   // 1

   // register some BPs to gain 33% of activated share
   while (get_activated_share() < 33 && registered_prods < producers.size()) {
      const auto& p = producers[registered_prods++];

      create_account_with_resources(p, config::system_account_name, STRSYM("10.0000"), false, STRSYM("100.0000"), STRSYM("100.0000"), vote_stake, true);
      regproducer(p);
      vote(p, {p});

      produce_block(fc::hours(25));
      produce_blocks(10);
   }

   // check registered & active BPs (optional)
   BOOST_TEST_MESSAGE("activated_share = " << get_activated_share()
      << "; registered BPs = " << registered_prods
      << "; active BPs = " << active_producers_num()
      << "; head_block_num = " << head_block_num());
   BOOST_TEST(registered_prods == 50);
   BOOST_TEST(active_producers_num() == 21);

   // wait until 21 BPs are activated; other BPs should not be activated
   while (active_producers_num() < 21) {
      produce_blocks(10);
   }
   BOOST_TEST_MESSAGE("head_block_num = " << head_block_num() << "; active BPs: " << active_producers_num());

   // generate some more blocks: schedule still contains 21 BPs
   for (size_t i = 0; i < 15; i++) {
      produce_blocks(50);
      BOOST_REQUIRE_EQUAL(21, active_producers_num());
   }

   // 2

   //FIXME: only 21 BPs are checked below; increase their number

   // add mode BPs
   while (get_activated_share() < 40 && registered_prods < producers.size()) {
      const auto& p = producers[registered_prods++];

      create_account_with_resources(p, config::system_account_name, STRSYM("10.0000"), false, STRSYM("100.0000"), STRSYM("100.0000"), vote_stake, true);
      regproducer(p);
      vote(p, {p});

      BOOST_TEST_MESSAGE("BP registered; activated_share = " << get_activated_share()
         << "; registered BPs = " << registered_prods
         << "; active BPs = " << active_producers_num());

      produce_block(fc::hours(25));
      produce_blocks(10);
   }

   // create rest producers
   while (registered_prods < producers.size()) {
      const auto& p = producers[registered_prods++];
      create_account_with_resources(p, config::system_account_name, STRSYM("10.0000"), false, STRSYM("100.0000"), STRSYM("100.0000"), vote_stake, true);
      regproducer(p);
      vote(p, {p});
      BOOST_TEST_MESSAGE("producer " << (registered_prods-1) << " created");
   }

   // wait until target_producers_num BPs are activated; other BPs should not be activated
   const size_t target_producers_num = 21 + (get_activated_share() - 33) * 3; // see voting.cpp
   BOOST_TEST_MESSAGE("wait for " << target_producers_num << " active BPs...");
   while (active_producers_num() < target_producers_num) {
      produce_blocks(50);
      BOOST_TEST_MESSAGE("active BPs: " << active_producers_num());
   }
   BOOST_TEST_MESSAGE("activated_share = " << get_activated_share()
      << "; registered BPs = " << registered_prods
      << "; active BPs = " << active_producers_num()
      << "; head_block_num = " << head_block_num());

   BOOST_REQUIRE_EQUAL(target_producers_num, active_producers_num());

   BOOST_TEST_MESSAGE("state: " << variant_to_string(get_global_state()));
   BOOST_TEST_MESSAGE("active stake = " << get_global_state()["active_stake"].as<int64_t>() <<
                      "; total stake = " << get_token_supply().to_string());

   BOOST_TEST_MESSAGE("XXX: active schedule size = " << control->head_block_state()->active_schedule.producers.size());

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
