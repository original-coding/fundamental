#include <limits>
#include <random>

namespace Fundamental {

template <typename NumberType, typename UniformRandomNumberGenerator = std::mt19937>
class RandomGenerator final {
public:
    using GenNumberType = NumberType;

public:
    constexpr RandomGenerator(UniformRandomNumberGenerator&& gen,
                              NumberType _min = std::numeric_limits<NumberType>::min(),
                              NumberType _max = std::numeric_limits<NumberType>::max()) :
    distribution(std::uniform_int_distribution<NumberType>(_min, _max)),
    generator(std::forward<UniformRandomNumberGenerator>(gen)) {
    }
    constexpr RandomGenerator(const UniformRandomNumberGenerator& gen,
                              NumberType _min = std::numeric_limits<NumberType>::min(),
                              NumberType _max = std::numeric_limits<NumberType>::max()) :
    distribution(std::uniform_int_distribution<NumberType>(_min, _max)), generator(gen) {
    }
    RandomGenerator(const RandomGenerator& other) : distribution(other.distribution), generator(other.generator) {
    }
    RandomGenerator(RandomGenerator&& other) :
    distribution(std::move(other.distribution)), generator(std::move(other.generator)) {
    }

    RandomGenerator& operator=(const RandomGenerator& other) {
        distribution = other.distribution;
        generator    = other.generator;
        return *this;
    }
    RandomGenerator& operator=(RandomGenerator&& other) {
        distribution = std::move(other.distribution);
        generator    = std::move(other.generator);
        return *this;
    }
    // gen random number
    [[nodiscard]] constexpr NumberType operator()() {
        return distribution(generator);
    }
    [[nodiscard]] constexpr NumberType gen() {
        return distribution(generator);
    }
    // gen random number array
    constexpr void gen(NumberType* dst, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            dst[i] = distribution(generator);
    }
    // gen another value type with multipe numerator/denominator
    template <typename U>
    [[nodiscard]] constexpr U multipe_gen(U numerator = static_cast<U>(1), U denominator = static_cast<U>(1)) {
        return static_cast<U>(distribution(generator)) * numerator / denominator;
    }
    template <typename U>
    constexpr void multipe_gen(U* dst, std::size_t count, U numerator = static_cast<U>(1),
                               U denominator = static_cast<U>(1)) {
        for (std::size_t i = 0; i < count; ++i)
            dst[i] = static_cast<U>(distribution(generator)) * numerator / denominator;
    }

private:
    std::uniform_int_distribution<NumberType> distribution;
    UniformRandomNumberGenerator generator;
};

[[nodiscard]] inline std::mt19937 DefaultGenerator() {
    static std::random_device rd;
    auto seed_data = std::array<int, std::mt19937::state_size> {};
    std::generate(std::begin(seed_data), std::end(seed_data), std::ref(rd));
    std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
    return std::mt19937(seq);
}

template <typename NumberType>
[[nodiscard]] inline RandomGenerator<NumberType> DefaultNumberGenerator(
    NumberType _min = std::numeric_limits<NumberType>::min(),
    NumberType _max = std::numeric_limits<NumberType>::max()) {
    return RandomGenerator<NumberType>(DefaultGenerator(), _min, _max);
}
} // namespace Fundamental