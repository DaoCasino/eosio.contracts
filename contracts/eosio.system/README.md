eosio.system
----------

This contract provides multiple functionalities:
- Users can stake tokens for CPU, Network and VOTE bandwidth, and then vote for producers or delegate their vote to a proxy.
- Producers register in order to be voted for, and can claim per-block and per-vote rewards.
- Users can buy and sell RAM at a market-determined price.
- Users can bid on premium names.

Actions:
The naming convention is codeaccount::actionname followed by a list of paramters.

## eosio::regproducer producer producer_key url location
   - Indicates that a particular account wishes to become a producer
   - **producer** account registering to be a producer candidate
   - **producer_key** producer account public key
   - **url** producer URL
   - **location** currently unused index

## eosio::voteproducer voter proxy producers
   - **voter** the account doing the voting
   - **proxy** proxy account to whom voter delegates vote
   - **producers** list of producers voted for. A maximum of 30 producers is allowed
   - Voter can vote for a proxy __or__ a list of at most 30 producers. Storage change is billed to `voter`.

## eosio::regproxy proxy is_proxy
   - **proxy** the account registering as voter proxy (or unregistering)
   - **is_proxy** if true, proxy is registered; if false, proxy is unregistered
   - Storage change is billed to `proxy`.

## eosio::delegatebw from receiver stake\_net\_quantity stake\_cpu\_quantity stake\_vote\_quantity transfer
   - **from** account holding tokens to be staked
   - **receiver** account to whose resources staked tokens are added
   - **stake\_net\_quantity** tokens staked for NET bandwidth
   - **stake\_cpu\_quantity** tokens staked for CPU bandwidth
   - **stake\_vote\_quantity** tokens staked for VOTE bandwidth
   - **transfer** if true, ownership of staked tokens is transfered to `receiver`
   - All producers `from` account has voted for will have their votes updated immediately.

## eosio::undelegatebw from receiver unstake\_net\_quantity unstake\_cpu\_quantity
   - **from** account whose tokens will be unstaked
   - **receiver** account to whose benefit tokens have been staked
   - **unstake\_net\_quantity** tokens to be unstaked from NET bandwidth
   - **unstake\_cpu\_quantity** tokens to be unstaked from CPU bandwidth
   - **unstake\_vote\_quantity** tokens to be unstaked from CPU bandwidth
   - Unstaked tokens are transferred to `from` liquid balance via a deferred transaction with a delay of 14 days.
   - If called during the delay period of a previous `undelegatebw` action, pending action is canceled and timer is reset.
   - All producers `from` account has voted for will have their votes updated immediately.
   - Bandwidth and storage for the deferred transaction are billed to `from`.

## eosio::onblock header
   - This special action is triggered when a block is applied by a given producer, and cannot be generated from
     any other source. It is used increment the number of unpaid blocks by a producer and update producer schedule.

## eosio::claimrewards producer
   - **producer** producer account claiming per-block and per-vote rewards
