/**
 * @file layers.h
 * @brief TCP next_layer and lowest_layer customization.
 */

#pragma once
#ifndef BNIO_TCP_LAYERS_H_
#define BNIO_TCP_LAYERS_H_

namespace bnio {

/**
 * Returns the next protocol layer for a stream-like object.
 */
template <class Layer>
[[nodiscard]] decltype(auto) get_next_layer(Layer& layer) noexcept {
  return layer.next_layer();
}

/**
 * Returns the next protocol layer for a const stream-like object.
 */
template <class Layer>
[[nodiscard]] decltype(auto) get_next_layer(const Layer& layer) noexcept {
  return layer.next_layer();
}

/**
 * Returns the lowest protocol layer for a stream-like object.
 */
template <class Layer>
[[nodiscard]] decltype(auto) get_lowest_layer(Layer& layer) noexcept {
  return layer.lowest_layer();
}

/**
 * Returns the lowest protocol layer for a const stream-like object.
 */
template <class Layer>
[[nodiscard]] decltype(auto) get_lowest_layer(const Layer& layer) noexcept {
  return layer.lowest_layer();
}

}  // namespace bnio

#endif  // BNIO_TCP_LAYERS_H_
