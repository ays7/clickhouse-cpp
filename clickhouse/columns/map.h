#pragma once

#include "array.h"
#include "column.h"
#include "tuple.h"

#include <map>

namespace clickhouse {

template <typename KeyColumnType, typename ValueColumnType>
class ColumnMapT;

/**
 * Represents column of Map(K, V).
 */
class ColumnMap : public Column {
public:
    /** Create a map of given type, with actual values and offsets.
     *
     *  Both `data` and `offsets` are used (and modified) internally bye ColumnArray.
     *  Users are strongly advised against modifying contents of `data` or `offsets` afterwards.
     */
    explicit ColumnMap(ColumnRef data);

    /// Appends content of given column to the end of current one.
    void Append(ColumnRef column) override;

    /// Loads column prefix from input stream.
    bool LoadPrefix(InputStream* input, size_t rows) override;

    /// Loads column data from input stream.
    bool LoadBody(InputStream* input, size_t rows) override;

    /// Saves column prefix to output stream.
    void SavePrefix(OutputStream* output) override;

    /// Saves column data to output stream.
    void SaveBody(OutputStream* output) override;

    /// Clear column data .
    void Clear() override;

    /// Returns count of rows in the column.
    size_t Size() const override;

    /// Makes slice of the current column.
    ColumnRef Slice(size_t, size_t) const override;
    ColumnRef CloneEmpty() const override;
    void Swap(Column&) override;

    /// Converts map at pos n to column.
    /// Type of row is tuple {key, value}.
    ColumnRef GetAsColumn(size_t n) const;

protected:
    template <typename K, typename V>
    friend class ColumnMapT;

    ColumnMap(ColumnMap&& map);

private:
    std::shared_ptr<ColumnArray> data_;
};

template <typename K, typename V>
class ColumnMapT : public ColumnMap {
public:
    using KeyColumnType   = K;
    using ValueColumnType = V;
    using Key             = std::decay_t<decltype(std::declval<KeyColumnType>().At(0))>;
    using Value           = std::decay_t<decltype(std::declval<ValueColumnType>().At(0))>;
    using TupleColumnType = ColumnTupleT<KeyColumnType, ValueColumnType>;
    using ArrayColumnType = ColumnArrayT<TupleColumnType>;

    ColumnMapT(ColumnRef data)
        : ColumnMap(data), typed_data_(data->AsStrict<ColumnArrayT<TupleColumnType>>()) {}

    ColumnMapT(std::shared_ptr<KeyColumnType> keys, std::shared_ptr<ValueColumnType> values)
        : ColumnMap(std::make_shared<ArrayColumnType>(std::make_shared<TupleColumnType>(
              std::make_tuple(std::move(keys), std::move(values))))),
          typed_data_(data_->template As<ArrayColumnType>()) {}

    ColumnRef Slice(size_t begin, size_t len) const override {
        return std::make_shared<ColumnMapT<K, V>>(typed_data_->Slice(begin, len));
    }

    ColumnRef CloneEmpty() const override {
        return std::make_shared<ColumnMapT<K, V>>(typed_data_->CloneEmpty());
    }

    void Swap(Column& other) override {
        auto& col = dynamic_cast<ColumnMapT<K, V>&>(other);
        col.typed_data_.swap(typed_data_);
        ColumnMap::Swap(other);
    }

    /// A single (row) value of the Map-column i.e. read-only map.
    /// It has a linear time complexity to access items
    /// Because data base type has same structure
    /// "This lookup works now with a linear complexity."
    /// https://clickhouse.com/docs/en/sql-reference/data-types/map
    /// Convert it to a suitable container required to access more than one element

    class MapValueView {
        const typename ArrayColumnType::ArrayValueView data_;

    public:
        using ValueType = std::pair<Key, Value>;

        MapValueView(typename ArrayColumnType::ArrayValueView data) : data_(std::move(data)) {}

        inline auto operator[](const Key& key) const { return (*Find(key)).second; }

        inline auto At(const Key& key) const {
            auto it = Find(key);
            if (it == end()) throw ValidationError("ColumnMap value key not found");
            return (*it).second;
        }

        class Iterator {
            typename ArrayColumnType::ArrayValueView::Iterator data_iterator_;

        public:
            Iterator(typename ArrayColumnType::ArrayValueView::Iterator data_iterator)
                : data_iterator_(data_iterator) {}

            using ValueType = std::pair<Key, Value>;
            using difference_type = size_t;
            using value_type = ValueType;
            using pointer = void;
            using reference = ValueType&;
            using iterator_category = std::forward_iterator_tag;

            inline auto operator*() const {
                auto tuple = *data_iterator_;
                return ValueType{std::get<0>(tuple), std::get<1>(tuple)};
            }

            inline Iterator& operator++() {
                ++data_iterator_;
                return *this;
            }

            inline bool operator==(const Iterator& other) const {
                return this->data_iterator_ == other.data_iterator_;
            }

            inline bool operator!=(const Iterator& other) const { return !(*this == other); }
        };

        // minimalistic stl-like container interface, hence the lowercase
        inline Iterator begin() const { return Iterator{data_.begin()}; }

        inline Iterator cbegin() const { return Iterator{data_.cbegin()}; }

        inline Iterator end() const { return Iterator{data_.end()}; }

        inline Iterator cend() const { return Iterator{data_.cend()}; }

        inline size_t size() const { return data_.size(); }

        // It is ugly to have both size() and Size(), but it is for compatitability with both STL
        // and rest of the clickhouse-cpp.
        inline size_t Size() const { return data_.Size(); }

        inline size_t Count(const Key& key) const {
            size_t result = 0;
            for (auto item : data_) {
                if (std::get<0>(item) == key) {
                    ++result;
                }
            }
            return result;
        }

        inline Iterator Find(const Key& key) const {
            for (auto it = data_.begin(); it != data_.end(); ++it) {
                if (std::get<0>(*it) == key) {
                    return Iterator{it};
                }
            }
            return end();
        }

        inline bool operator==(const MapValueView& other) const {
            if (size() != other.size()) {
                return false;
            }
            using Vector = std::vector<std::pair<Key, Value>>;
            Vector l(begin(), end());
            Vector r(other.begin(), other.end());
            auto comp = [](const auto& l, const auto& r) { return l.frist < r.first; };
            std::sort(l.begin(), l.end(), comp);
            std::sort(r.begin(), r.end(), comp);
            return std::equal(l.begin(), l.end(), r.begin(), r.end());
            return true;
        }

        inline bool operator!=(const MapValueView& other) const { return !(*this == other); }
    };

    inline auto At(size_t index) const { return MapValueView{typed_data_->At(index)}; }

    inline auto operator[](size_t index) const { return MapValueView{typed_data_->At(index)}; }

    using ColumnMap::Append;

    inline void Append(const MapValueView& value) { typed_data_->Append(value.data_); }

    inline void Append(const std::vector<std::tuple<Key, Value>>& tuples) {
        typed_data_->Append(tuples.begin(), tuples.end());
    }

    template <typename T>
    inline void Append(const T& value) {
        // TODO  Refuse to copy.
        std::vector<std::tuple<typename T::key_type, typename T::mapped_type>> container;
        container.reserve(value.size());
        for (const auto& i : value) {
            container.emplace_back(i.first, i.second);
        }
        typed_data_->Append(container.begin(), container.end());
    }

    static auto Wrap(ColumnMap&& col) {
        auto data = ArrayColumnType::Wrap(std::move(col.data_));
        return std::make_shared<ColumnMapT<K, V>>(std::move(data));
    }

    static auto Wrap(Column&& col) { return Wrap(std::move(dynamic_cast<ColumnMap&&>(col))); }

    // Helper to simplify integration with other APIs
    static auto Wrap(ColumnRef&& col) { return Wrap(std::move(*col->AsStrict<ColumnMap>())); }

private:
    std::shared_ptr<ArrayColumnType> typed_data_;
};

}  // namespace clickhouse
