#include "lob/order_book.hpp"

#include <algorithm>
#include <cstdint>

namespace lob {

namespace {

constexpr std::size_t kDefaultLevelPoolDivisor = 4;

}  // namespace

void PriceLevel::push_back(OrderNode* node) noexcept {
    node->prev = tail;
    node->next = nullptr;
    node->level = this;

    if (tail != nullptr) {
        tail->next = node;
    } else {
        head = node;
    }
    tail = node;

    ++order_count;
    total_qty += node->qty;
}

void PriceLevel::erase(OrderNode* node) noexcept {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        head = node->next;
    }

    if (node->next != nullptr) {
        node->next->prev = node->prev;
    } else {
        tail = node->prev;
    }

    --order_count;
    total_qty -= node->qty;

    node->prev = nullptr;
    node->next = nullptr;
    node->level = nullptr;
}

void PriceLevel::reset(Price new_price) noexcept {
    price = new_price;
    total_qty = 0;
    order_count = 0;
    head = nullptr;
    tail = nullptr;
}

OrderNodePool::OrderNodePool(std::size_t capacity)
    : storage_(capacity),
      free_stack_(capacity) {
    for (std::size_t i = 0; i < capacity; ++i) {
        free_stack_[capacity - 1 - i] = i;
    }
}

OrderNode* OrderNodePool::allocate() noexcept {
    if (free_stack_.empty()) {
        return nullptr;
    }

    const std::size_t index = free_stack_.back();
    free_stack_.pop_back();
    OrderNode* node = &storage_[index];
    *node = OrderNode{};
    return node;
}

void OrderNodePool::release(OrderNode* node) noexcept {
    if (node == nullptr || storage_.empty()) {
        return;
    }

    const auto* base = storage_.data();
    const auto* end = base + storage_.size();
    if (node < base || node >= end) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(node - base);
    *node = OrderNode{};
    free_stack_.push_back(index);
}

PriceLevelPool::PriceLevelPool(std::size_t capacity)
    : storage_(capacity),
      free_stack_(capacity) {
    for (std::size_t i = 0; i < capacity; ++i) {
        free_stack_[capacity - 1 - i] = i;
    }
}

PriceLevel* PriceLevelPool::allocate(Price price) noexcept {
    if (free_stack_.empty()) {
        return nullptr;
    }

    const std::size_t index = free_stack_.back();
    free_stack_.pop_back();
    PriceLevel* level = &storage_[index];
    level->reset(price);
    return level;
}

void PriceLevelPool::release(PriceLevel* level) noexcept {
    if (level == nullptr || storage_.empty()) {
        return;
    }

    const auto* base = storage_.data();
    const auto* end = base + storage_.size();
    if (level < base || level >= end) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(level - base);
    level->reset(0);
    free_stack_.push_back(index);
}

OrderBook::OrderBook(std::size_t expected_orders)
    : order_pool_(expected_orders),
      level_pool_(std::max<std::size_t>(32, expected_orders / kDefaultLevelPoolDivisor)) {
    orders_by_id_.reserve(expected_orders);
    levels_[0].reserve(expected_orders / kDefaultLevelPoolDivisor);
    levels_[1].reserve(expected_orders / kDefaultLevelPoolDivisor);
}

bool OrderBook::add_order(OrderId id, Side side, Price price, Quantity qty) {
    if (qty == 0 || orders_by_id_.find(id) != orders_by_id_.end()) {
        return false;
    }

    OrderNode* node = order_pool_.allocate();
    if (node == nullptr) {
        return false;
    }
    node->id = id;
    node->price = price;
    node->qty = qty;
    node->side = side;

    PriceLevel* level = get_or_create_level(side, price);
    if (level == nullptr) {
        order_pool_.release(node);
        return false;
    }

    level->push_back(node);

    if (side == Side::Buy) {
        if (!has_bid_ || price > best_bid_) {
            best_bid_ = price;
            has_bid_ = true;
        }
    } else {
        if (!has_ask_ || price < best_ask_) {
            best_ask_ = price;
            has_ask_ = true;
        }
    }

    orders_by_id_.emplace(id, node);
    return true;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_by_id_.find(id);
    if (it == orders_by_id_.end()) {
        return false;
    }

    remove_node(it->second);
    return true;
}

bool OrderBook::modify_order(OrderId id, Quantity new_qty) {
    auto it = orders_by_id_.find(id);
    if (it == orders_by_id_.end() || new_qty == 0) {
        return false;
    }

    OrderNode* node = it->second;
    PriceLevel* level = node->level;

    if (new_qty == node->qty) {
        return true;
    }

    if (new_qty > node->qty) {
        level->total_qty += (new_qty - node->qty);
    } else {
        level->total_qty -= (node->qty - new_qty);
    }

    node->qty = new_qty;
    return true;
}

bool OrderBook::replace_order(OrderId id, Price new_price, Quantity new_qty) {
    auto it = orders_by_id_.find(id);
    if (it == orders_by_id_.end() || new_qty == 0) {
        return false;
    }

    OrderNode* node = it->second;
    const Price old_price = node->price;
    const Quantity old_qty = node->qty;

    // Same-price quantity reduction preserves FIFO priority.
    if (new_price == old_price && new_qty <= old_qty) {
        return modify_order(id, new_qty);
    }

    // Price change or size increase is treated as cancel + new add (priority reset).
    const Side side = node->side;
    remove_node(node);
    return add_order(id, side, new_price, new_qty);
}

MatchResult OrderBook::process_limit_order(OrderId id, Side side, Price price, Quantity qty) {
    MatchResult result{};
    if (qty == 0 || orders_by_id_.find(id) != orders_by_id_.end()) {
        result.remaining_qty = qty;
        return result;
    }

    Quantity remaining = qty;
    const Side passive_side = (side == Side::Buy) ? Side::Sell : Side::Buy;

    while (remaining > 0 && has_cross(side, price)) {
        const Price match_price = best_price_for(passive_side);
        while (remaining > 0) {
            auto& passive_levels = levels_for(passive_side);
            auto level_it = passive_levels.find(match_price);
            if (level_it == passive_levels.end()) {
                break;
            }

            PriceLevel* level = level_it->second;
            if (level->empty()) {
                break;
            }

            OrderNode* passive_node = level->head;
            const Quantity traded = std::min(remaining, passive_node->qty);

            Fill fill{};
            fill.passive_order_id = passive_node->id;
            fill.aggressive_order_id = id;
            fill.price = passive_node->price;
            fill.qty = traded;
            if (result.fill_count < MatchResult::kMaxFills) { result.fills[result.fill_count++] = fill; }

            remaining -= traded;
            result.filled_qty += traded;

            if (traded == passive_node->qty) {
                remove_node(passive_node);
            } else {
                passive_node->qty -= traded;
                level->total_qty -= traded;
            }
        }
    }

    result.remaining_qty = remaining;
    if (remaining > 0) {
        result.rested = add_order(id, side, price, remaining);
        result.priority_preserved = false;
    }

    return result;
}

MatchResult OrderBook::process_market_order(OrderId id, Side side, Quantity qty) {
    MatchResult result{};
    if (qty == 0 || orders_by_id_.find(id) != orders_by_id_.end()) {
        result.remaining_qty = qty;
        return result;
    }

    Quantity remaining = qty;
    const Side passive_side = (side == Side::Buy) ? Side::Sell : Side::Buy;

    while (remaining > 0) {
        const Price match_price = best_price_for(passive_side);
        if ((passive_side == Side::Sell && !has_ask_) ||
            (passive_side == Side::Buy && !has_bid_)) {
            break;
        }

        auto& passive_levels = levels_for(passive_side);
        auto level_it = passive_levels.find(match_price);
        if (level_it == passive_levels.end()) {
            break;
        }

        PriceLevel* level = level_it->second;
        while (remaining > 0 && !level->empty()) {
            OrderNode* passive_node = level->head;
            const Quantity traded = std::min(remaining, passive_node->qty);

            Fill fill{};
            fill.passive_order_id = passive_node->id;
            fill.aggressive_order_id = id;
            fill.price = passive_node->price;
            fill.qty = traded;
            if (result.fill_count < MatchResult::kMaxFills) { result.fills[result.fill_count++] = fill; }

            remaining -= traded;
            result.filled_qty += traded;

            if (traded == passive_node->qty) {
                remove_node(passive_node);
            } else {
                passive_node->qty -= traded;
                level->total_qty -= traded;
            }
        }
    }

    result.remaining_qty = remaining;
    return result;
}

const OrderNode* OrderBook::find_order(OrderId id) const {
    auto it = orders_by_id_.find(id);
    return (it == orders_by_id_.end()) ? nullptr : it->second;
}

Price OrderBook::best_bid() const noexcept {
    return has_bid_ ? best_bid_ : 0;
}

Price OrderBook::best_ask() const noexcept {
    return has_ask_ ? best_ask_ : 0;
}

std::size_t OrderBook::live_order_count() const noexcept {
    return orders_by_id_.size();
}

std::vector<OrderId> OrderBook::orders_at_level(Side side, Price price) const {
    std::vector<OrderId> ids;
    auto level_it = levels_for(side).find(price);
    if (level_it == levels_for(side).end()) {
        return ids;
    }

    const PriceLevel* level = level_it->second;
    ids.reserve(level->order_count);
    for (const OrderNode* node = level->head; node != nullptr; node = node->next) {
        ids.push_back(node->id);
    }
    return ids;
}

PriceLevel* OrderBook::get_or_create_level(Side side, Price price) {
    auto& levels = levels_for(side);
    auto it = levels.find(price);
    if (it != levels.end()) {
        return it->second;
    }

    PriceLevel* level = level_pool_.allocate(price);
    if (level == nullptr) {
        return nullptr;
    }

    auto [inserted_it, inserted] = levels.emplace(price, level);
    if (!inserted) {
        level_pool_.release(level);
    } else {
        auto& sorted = sorted_levels_[static_cast<std::size_t>(side)];
        sorted.insert(std::lower_bound(sorted.begin(), sorted.end(), price), price);
    }
    return inserted_it->second;
}

OrderBook::LevelMap& OrderBook::levels_for(Side side) noexcept {
    return levels_[static_cast<std::size_t>(side)];
}

const OrderBook::LevelMap& OrderBook::levels_for(Side side) const noexcept {
    return levels_[static_cast<std::size_t>(side)];
}

void OrderBook::refresh_best_bid_after_remove(Price removed_price) {
    if (!has_bid_ || removed_price != best_bid_) {
        return;
    }

    auto& sorted = sorted_levels_[static_cast<std::size_t>(Side::Buy)];
    if (sorted.empty()) {
        best_bid_ = 0;
        has_bid_ = false;
        return;
    }

    best_bid_ = sorted.back();
}

void OrderBook::refresh_best_ask_after_remove(Price removed_price) {
    if (!has_ask_ || removed_price != best_ask_) {
        return;
    }

    auto& sorted = sorted_levels_[static_cast<std::size_t>(Side::Sell)];
    if (sorted.empty()) {
        best_ask_ = 0;
        has_ask_ = false;
        return;
    }

    best_ask_ = sorted.front();
}

bool OrderBook::has_cross(Side aggressive_side, Price aggressive_price) const noexcept {
    if (aggressive_side == Side::Buy) {
        return has_ask_ && aggressive_price >= best_ask_;
    }
    return has_bid_ && aggressive_price <= best_bid_;
}

Price OrderBook::best_price_for(Side side) const noexcept {
    return (side == Side::Buy) ? best_bid_ : best_ask_;
}

void OrderBook::remove_node(OrderNode* node) {
    if (node == nullptr || node->level == nullptr) {
        return;
    }

    PriceLevel* level = node->level;
    const Side side = node->side;
    const Price removed_price = node->price;

    level->erase(node);
    orders_by_id_.erase(node->id);
    order_pool_.release(node);

    if (level->empty()) {
        levels_for(side).erase(removed_price);
        auto& sorted = sorted_levels_[static_cast<std::size_t>(side)];
        auto sorted_it = std::lower_bound(sorted.begin(), sorted.end(), removed_price);
        if (sorted_it != sorted.end() && *sorted_it == removed_price) {
            sorted.erase(sorted_it);
        }
        level_pool_.release(level);
        if (side == Side::Buy) {
            refresh_best_bid_after_remove(removed_price);
        } else {
            refresh_best_ask_after_remove(removed_price);
        }
    }
}

}  // namespace lob
