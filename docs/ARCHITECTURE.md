# Architecture Notes

## 1. Matching Core

`OrderBook` is a single-writer data structure intended to be mutated by one thread at a time.

### Data Model

- `OrderNode`: Intrusive doubly linked list node, one node per resting order.
- `PriceLevel`: FIFO queue for orders at one price.
- `orders_by_id_`: Maps `OrderId -> OrderNode*` for O(1)-style lookup.
- `levels_`: Side-indexed map of `Price -> PriceLevel*`.

### Pool-Backed Storage

- `OrderNodePool`: Preallocated node storage plus freelist.
- `PriceLevelPool`: Preallocated level storage plus freelist.
- Allocation in hot paths uses pool freelists, not `new` per order/level.

## 2. Matching Semantics

### Limit Orders

`process_limit_order(id, side, price, qty)`:

- If crossing spread, consumes opposite side best prices first.
- At each price, consumes passive orders FIFO (`head` first).
- Partial fills supported.
- Remaining aggressive quantity rests on book at submitted limit price.

### Market Orders

`process_market_order(id, side, qty)`:

- Consumes opposite side top-of-book FIFO until qty is filled or book side is empty.
- Any unfilled remainder is returned and does not rest on book.

### Replace Rules

`replace_order(id, new_price, new_qty)`:

- Same price and reduced size: priority preserved (in-place modify).
- Price change or size increase: cancel + re-add, priority reset.

## 3. Runtime Concurrency Model

`MatchingEngineRuntime` runs `OrderBook` on a dedicated `std::jthread`.

### Command Queue

- Bounded SPSC ring buffer.
- Producer: caller thread(s) expected to behave as single producer for strict SPSC assumptions.
- Consumer: worker `jthread`.
- Full queue condition rejects command (backpressure signal).

### Memory Ordering

- Producer publishes writes with release on `write_idx_`.
- Consumer observes `write_idx_` with acquire.
- Consumer publishes `read_idx_` with release.
- Producer observes `read_idx_` with acquire for capacity checks.

### Runtime APIs

- Fire-and-forget commands: `submit_add/cancel/modify`.
- Request-response commands: `submit_replace`, `submit_limit_order`, `submit_market_order`.
- `sync()` uses a barrier command to ensure all prior commands are applied.
- `shutdown_and_snapshot()` ensures queue drain before final state read.

## 4. Testing Strategy

- `order_book_tests`: lifecycle, FIFO integrity, matching correctness.
- `runtime_tests`: backpressure and drain behavior, threaded Week 2 flow.

## 5. Current Limitations

- Multi-producer safety for queue is not implemented in the SPSC path.
- No persistence/WAL yet.
- No networking ingress/egress path yet.
- No dedicated risk checks yet.
