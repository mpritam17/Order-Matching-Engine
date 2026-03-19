#pragma once

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "ankerl/unordered_dense.h"
#include "lob/types.hpp"

namespace lob {

struct PriceLevel;

// Intrusive node that links directly into a per-price doubly linked FIFO list.
struct alignas(64) OrderNode {
    OrderId id{};
    Price price{};
    Quantity qty{};
    Side side{Side::Buy};

    OrderNode* prev{nullptr};
    OrderNode* next{nullptr};
    PriceLevel* level{nullptr};
};

struct alignas(64) PriceLevel {
    Price price{};
    Quantity total_qty{};
    std::size_t order_count{};

    OrderNode* head{nullptr};
    OrderNode* tail{nullptr};

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    void push_back(OrderNode* node) noexcept;
    void erase(OrderNode* node) noexcept;
    void reset(Price new_price) noexcept;
};

struct Fill {
    OrderId passive_order_id{};
    OrderId aggressive_order_id{};
    Price price{};
    Quantity qty{};
};

struct MatchResult {
    Quantity filled_qty{};
    Quantity remaining_qty{};
    bool rested{};
    bool priority_preserved{};
    std::vector<Fill> fills{};
};

class OrderNodePool {
public:
    explicit OrderNodePool(std::size_t capacity = 1 << 20);

    [[nodiscard]] OrderNode* allocate() noexcept;
    void release(OrderNode* node) noexcept;

private:
    std::vector<OrderNode> storage_{};
    std::vector<std::size_t> free_stack_{};
};

class PriceLevelPool {
public:
    explicit PriceLevelPool(std::size_t capacity = 1 << 18);

    [[nodiscard]] PriceLevel* allocate(Price price) noexcept;
    void release(PriceLevel* level) noexcept;

private:
    std::vector<PriceLevel> storage_{};
    std::vector<std::size_t> free_stack_{};
};

class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1 << 20);

    bool add_order(OrderId id, Side side, Price price, Quantity qty);
    bool cancel_order(OrderId id);
    bool modify_order(OrderId id, Quantity new_qty);
    bool replace_order(OrderId id, Price new_price, Quantity new_qty);
    MatchResult process_limit_order(OrderId id, Side side, Price price, Quantity qty);
    MatchResult process_market_order(OrderId id, Side side, Quantity qty);

    [[nodiscard]] const OrderNode* find_order(OrderId id) const;
    [[nodiscard]] Price best_bid() const noexcept;
    [[nodiscard]] Price best_ask() const noexcept;
    [[nodiscard]] std::vector<OrderId> orders_at_level(Side side, Price price) const;

    [[nodiscard]] std::size_t live_order_count() const noexcept;

private:
    using LevelMap = ankerl::unordered_dense::map<Price, PriceLevel*>;

    [[nodiscard]] PriceLevel* get_or_create_level(Side side, Price price);
    [[nodiscard]] LevelMap& levels_for(Side side) noexcept;
    [[nodiscard]] const LevelMap& levels_for(Side side) const noexcept;
    [[nodiscard]] bool has_cross(Side aggressive_side, Price aggressive_price) const noexcept;
    [[nodiscard]] Price best_price_for(Side side) const noexcept;
    void remove_node(OrderNode* node);

    std::array<LevelMap, 2> levels_{};
    ankerl::unordered_dense::map<OrderId, OrderNode*> orders_by_id_{};

    OrderNodePool order_pool_{};
    PriceLevelPool level_pool_{};

    Price best_bid_{0};
    Price best_ask_{0};
    bool has_bid_{false};
    bool has_ask_{false};

    void refresh_best_bid_after_remove(Price removed_price);
    void refresh_best_ask_after_remove(Price removed_price);
};

}  // namespace lob
