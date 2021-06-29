#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>
#include <cmath> // DAO: for pow()

namespace eosiosystem {

   using eosio::current_time_point;
   using eosio::microseconds;
   using eosio::token;

   void system_contract::onblock( ignore<block_header> ) {
      using namespace eosio;

      require_auth(get_self());

      block_timestamp timestamp;
      name producer;
      _ds >> timestamp >> producer;

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      _gstate2.last_block_num = timestamp;

      /// Until activation, no new rewards are paid.
      if( _gstate.thresh_activated_stake_time == time_point() ) {
         return;
      }

      if( _gstate.last_pervote_bucket_fill == time_point() ) { // start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();
      }

      /// At startup, the initial producer may not be one that is registered / elected
      /// and therefore there may be no producer object for them.
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      // only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_elected_producers( timestamp );
         //ADD_DEBUG_LOG_MSG("prods updated");

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(get_self(), get_self().value);
            auto idx = bids.get_index<"highbid"_n>();
            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );

            //ADD_DEBUG_LOG_MSG(std::string("closing finished bids") +
            //   "; highest found?: " + (highest != idx.end() ? "y" : "n") +
            //   (highest != idx.end()
            //      ? ",\n  highest->high_bid = "                   + std::to_string(highest->high_bid) +
            //        ",\n  current_time_point() = "                + std::to_string(current_time_point().sec_since_epoch()) +
            //        ",\n  highest->last_bid_time = "              + std::to_string(highest->last_bid_time.sec_since_epoch()) +
            //        ",\n  microseconds(useconds_per_day) = "      + std::to_string(microseconds(useconds_per_day).to_seconds()) +
            //        ",\n  _gstate.thresh_activated_stake_time = " + std::to_string(_gstate.thresh_activated_stake_time.sec_since_epoch()) +
            //        ",\n  time_point() = "                        + std::to_string(time_point().sec_since_epoch()) +
            //        ",\n  microseconds(14 * useconds_per_day) = " + std::to_string(microseconds(14 * useconds_per_day).to_seconds())
            //      : "")
            //);
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               //ADD_DEBUG_LOG_MSG("bid closed");
               _gstate.last_name_close = timestamp;
               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }

   ///@{
   ///DAO
   double get_target_emission_rate_per_year(double activated_share) {
      if (activated_share <= 0.33) {
         return 0.2;
      } else if (activated_share >= 0.66) {
         return 0.1;
      }
      // descending linear function from 0.33 to 0.66
      return -10. / 33 * (activated_share - 0.33) + 0.2;
   }

   ///DAO: continuous rate formulae (#4)
   double get_continuous_rate(double emission_rate) {
         return (pow(1 + emission_rate, 1./blocks_per_hour) - 1) * blocks_per_hour;
   }
   ///@}

   void system_contract::claimrewards( const name& owner ) {
      require_auth( owner );

      const auto& prod = _producers.get( owner.value );
      check( prod.active(), "producer does not have an active key" );

      check( _gstate.thresh_activated_stake_time != time_point(),
                    "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)" );

      const auto ct = current_time_point();

      check( ct - prod.last_claim_time > microseconds(useconds_per_day), "already claimed rewards within past day" );

      const asset token_supply = token::get_supply(token_account, core_symbol().code() );
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();

      if( usecs_since_last_fill > 0 && _gstate.last_pervote_bucket_fill > time_point() ) {
         ///@{
         ///DAO: continuous rate formulae (#4); rewards
         double emission_rate = get_target_emission_rate_per_year(1.0 * _gstate.active_stake / token_supply.amount);
         double continuous_rate = get_continuous_rate(emission_rate);
         auto new_tokens = static_cast<int64_t>(continuous_rate * token_supply.amount * usecs_since_last_fill / useconds_per_year);
         auto to_dao           = new_tokens / 5; // goes to eosio.saving account
         auto to_producers     = new_tokens - to_dao;
         auto to_per_block_pay = to_producers / 4;
         auto to_per_vote_pay  = to_producers - to_per_block_pay;
         ///@}

         if( new_tokens > 0 ) {
            {
               token::issue_action issue_act{ token_account, { {get_self(), active_permission} } };
               issue_act.send( get_self(), asset(new_tokens, core_symbol()), "issue tokens for producer pay and DAO" ); // DAO
            }
            {
               token::transfer_action transfer_act{ token_account, { {get_self(), active_permission} } };
               if( to_dao > 0 ) { // DAO
                  transfer_act.send( get_self(), saving_account, asset(to_dao, core_symbol()), "reward for DAO" );
               }
               if( to_per_block_pay > 0 ) {
                  transfer_act.send( get_self(), bpay_account, asset(to_per_block_pay, core_symbol()), "fund per-block bucket" );
               }
               if( to_per_vote_pay > 0 ) {
                  transfer_act.send( get_self(), vpay_account, asset(to_per_vote_pay, core_symbol()), "fund per-vote bucket" );
               }
            }
         }

         _gstate.pervote_bucket          += to_per_vote_pay;
         _gstate.perblock_bucket         += to_per_block_pay;
         _gstate.last_pervote_bucket_fill = ct;
      }

      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
      /// time duration the vote weight has been held into one metric.
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
      // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
      // This is okay because in this case the producer will not get paid anything either way.
      // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.

      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = (_gstate.perblock_bucket * prod.unpaid_blocks) / _gstate.total_unpaid_blocks;
      }

      double new_votepay_share = update_producer_votepay_share( prod2,
                                    ct,
                                    updated_after_threshold ? 0.0 : prod.total_votes,
                                    true // reset votepay_share to zero after updating
                                 );

      int64_t producer_per_vote_pay = 0;
      if( _gstate2.revision > 0 ) {
         double total_votepay_share = update_total_votepay_share( ct );
         if( total_votepay_share > 0 && !crossed_threshold ) {
            producer_per_vote_pay = int64_t((new_votepay_share * _gstate.pervote_bucket) / total_votepay_share);
            if( producer_per_vote_pay > _gstate.pervote_bucket ) {
               producer_per_vote_pay = _gstate.pervote_bucket;
            }
         }
      } else {
         if( _gstate.total_producer_vote_weight > 0 ) {
            producer_per_vote_pay = int64_t((_gstate.pervote_bucket * prod.total_votes) / _gstate.total_producer_vote_weight);
         }
      }

      if( producer_per_vote_pay < min_pervote_daily_pay ) {
         producer_per_vote_pay = 0;
      }

      _gstate.pervote_bucket      -= producer_per_vote_pay;
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );

      _producers.modify( prod, same_payer, [&](auto& p) {
         p.last_claim_time = ct;
         p.unpaid_blocks   = 0;
      });

      if ( producer_per_block_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {bpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( bpay_account, owner, asset(producer_per_block_pay, core_symbol()), "producer block pay" );
      }
      if ( producer_per_vote_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {vpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( vpay_account, owner, asset(producer_per_vote_pay, core_symbol()), "producer vote pay" );
      }
   }

} //namespace eosiosystem
