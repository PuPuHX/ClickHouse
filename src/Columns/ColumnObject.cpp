#include <Columns/ColumnObject.h>
#include <Columns/ColumnCompressed.h>
#include <DataTypes/Serializations/SerializationDynamic.h>
#include <IO/Operators.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromString.h>
#include <Common/Arena.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}

namespace
{

static const FormatSettings & getFormatSettings()
{
    static const FormatSettings settings;
    return settings;
}

static const std::shared_ptr<SerializationDynamic> & getDynamicSerialization()
{
    static const std::shared_ptr<SerializationDynamic> dynamic_serialization = std::make_shared<SerializationDynamic>();
    return dynamic_serialization;
}

}

ColumnObject::ColumnObject(
    std::unordered_map<String, MutableColumnPtr> typed_paths_,
    std::unordered_map<String, MutableColumnPtr> dynamic_paths_,
    MutableColumnPtr shared_data_,
    size_t max_dynamic_paths_,
    size_t max_dynamic_types_,
    const Statistics & statistics_)
    : shared_data(std::move(shared_data_))
    , max_dynamic_paths(max_dynamic_paths_)
    , max_dynamic_types(max_dynamic_types_)
    , statistics(statistics_)
{
    typed_paths.reserve(typed_paths_.size());
    for (auto & [path, column] : typed_paths_)
        typed_paths[path] = std::move(column);

    dynamic_paths.reserve(dynamic_paths_.size());
    for (auto & [path, column] : dynamic_paths_)
        dynamic_paths[path] = std::move(column);
}

ColumnObject::ColumnObject(
    std::unordered_map<String, MutableColumnPtr> typed_paths_, size_t max_dynamic_paths_, size_t max_dynamic_types_)
    : max_dynamic_paths(max_dynamic_paths_), max_dynamic_types(max_dynamic_types_)
{
    typed_paths.reserve(typed_paths_.size());
    for (auto & [path, column] : typed_paths_)
    {
        if (!column->empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected non-empty typed path column in ColumnObject constructor");
        typed_paths[path] = std::move(column);
    }

    MutableColumns paths_and_values;
    paths_and_values.emplace_back(ColumnString::create());
    paths_and_values.emplace_back(ColumnString::create());
    shared_data = ColumnArray::create(ColumnTuple::create(std::move(paths_and_values)));
}

ColumnObject::Ptr ColumnObject::create(
    const std::unordered_map<String, ColumnPtr> & typed_paths_,
    const std::unordered_map<String, ColumnPtr> & dynamic_paths_,
    const ColumnPtr & shared_data_,
    size_t max_dynamic_paths_,
    size_t max_dynamic_types_,
    const ColumnObject::Statistics & statistics_)
{
    std::unordered_map<String, MutableColumnPtr> mutable_typed_paths;
    mutable_typed_paths.reserve(typed_paths_.size());
    for (const auto & [path, column] : typed_paths_)
        mutable_typed_paths[path] = typed_paths_.at(path)->assumeMutable();

    std::unordered_map<String, MutableColumnPtr> mutable_dynamic_paths;
    mutable_dynamic_paths.reserve(dynamic_paths_.size());
    for (const auto & [path, column] : dynamic_paths_)
        mutable_dynamic_paths[path] = dynamic_paths_.at(path)->assumeMutable();

    return ColumnObject::create(std::move(mutable_typed_paths), std::move(mutable_dynamic_paths), shared_data_->assumeMutable(), max_dynamic_paths_, max_dynamic_types_, statistics_);
}

ColumnObject::MutablePtr ColumnObject::create(
    std::unordered_map<String, MutableColumnPtr> typed_paths_,
    std::unordered_map<String, MutableColumnPtr> dynamic_paths_,
    MutableColumnPtr shared_data_,
    size_t max_dynamic_paths_,
    size_t max_dynamic_types_,
    const ColumnObject::Statistics & statistics_)
{
    return Base::create(std::move(typed_paths_), std::move(dynamic_paths_), std::move(shared_data_), max_dynamic_paths_, max_dynamic_types_, statistics_);
}

ColumnObject::MutablePtr ColumnObject::create(std::unordered_map<String, MutableColumnPtr> typed_paths_, size_t max_dynamic_paths_, size_t max_dynamic_types_)
{
    return Base::create(std::move(typed_paths_), max_dynamic_paths_, max_dynamic_types_);
}

std::string ColumnObject::getName() const
{
    WriteBufferFromOwnString ss;
    ss << "Object(";
    ss << "max_dynamic_paths=" << max_dynamic_paths;
    ss << ", max_dynamic_types=" << max_dynamic_types;
    std::vector<String> sorted_typed_paths;
    sorted_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        sorted_typed_paths.push_back(path);
    std::sort(sorted_typed_paths.begin(), sorted_typed_paths.end());
    for (const auto & path : sorted_typed_paths)
        ss << ", " << path << " " << typed_paths.at(path)->getName();
    ss << ")";
    return ss.str();
}

MutableColumnPtr ColumnObject::cloneEmpty() const
{
    std::unordered_map<String, MutableColumnPtr> empty_typed_paths;
    empty_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        empty_typed_paths[path] = column->cloneEmpty();

    std::unordered_map<String, MutableColumnPtr> empty_dynamic_paths;
    empty_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        empty_dynamic_paths[path] = column->cloneEmpty();

    return ColumnObject::create(std::move(empty_typed_paths), std::move(empty_dynamic_paths), shared_data->cloneEmpty(), max_dynamic_paths, max_dynamic_types, statistics);
}

MutableColumnPtr ColumnObject::cloneResized(size_t size) const
{
    std::unordered_map<String, MutableColumnPtr> resized_typed_paths;
    resized_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        resized_typed_paths[path] = column->cloneResized(size);

    std::unordered_map<String, MutableColumnPtr> resized_dynamic_paths;
    resized_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        resized_dynamic_paths[path] = column->cloneResized(size);

    return ColumnObject::create(std::move(resized_typed_paths), std::move(resized_dynamic_paths), shared_data->cloneResized(size), max_dynamic_paths, max_dynamic_types, statistics);
}

Field ColumnObject::operator[](size_t n) const
{
    Object object;

    for (const auto & [path, column] : typed_paths)
        object[path] = (*column)[n];
    for (const auto & [path, column] : dynamic_paths)
    {
        /// Output only non-null values from dynamic paths. We cannot distinguish cases when
        /// dynamic path has Null value and when it's absent in the row and consider them equivalent.
        if (!column->isNullAt(n))
            object[path] = (*column)[n];
    }

    const auto & shared_data_offsets = getSharedDataOffsets();
    const auto [shared_paths, shared_values] = getSharedDataPathsAndValues();
    size_t start = shared_data_offsets[ssize_t(n) - 1];
    size_t end = shared_data_offsets[n];
    for (size_t i = start; i != end; ++i)
    {
        String path = shared_paths->getDataAt(i).toString();
        auto value_data = shared_values->getDataAt(i);
        ReadBufferFromMemory buf(value_data.data, value_data.size);
        Field value;
        getDynamicSerialization()->deserializeBinary(value, buf, getFormatSettings());
        object[path] = value;
    }

    return object;
}

void ColumnObject::get(size_t n, Field & res) const
{
    res = (*this)[n];
}

bool ColumnObject::isDefaultAt(size_t n) const
{
    for (const auto & [path, column] : typed_paths)
    {
        if (!column->isDefaultAt(n))
            return false;
    }

    for (const auto & [path, column] : dynamic_paths)
    {
        if (!column->isDefaultAt(n))
            return false;
    }

    if (!shared_data->isDefaultAt(n))
        return false;

    return true;
}

StringRef ColumnObject::getDataAt(size_t) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method getDataAt is not supported for {}", getName());
}

void ColumnObject::insertData(const char *, size_t)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method insertData is not supported for {}", getName());
}

IColumn * ColumnObject::tryToAddNewDynamicPath(const String & path)
{
    if (dynamic_paths.size() == max_dynamic_paths)
        return nullptr;

    auto new_dynamic_column = ColumnDynamic::create(max_dynamic_types);
    new_dynamic_column->insertManyDefaults(size());
    auto it = dynamic_paths.emplace(path, std::move(new_dynamic_column)).first;
    return it->second.get();
}

void ColumnObject::setDynamicPaths(const std::vector<String> & paths)
{
    if (paths.size() > max_dynamic_paths)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot set dynamic paths to Object column, the number of paths ({}) exceeds the limit ({})", paths.size(), max_dynamic_paths);

    size_t size = this->size();
    for (const auto & path : paths)
    {
        auto new_dynamic_column = ColumnDynamic::create(max_dynamic_types);
        if (size)
            new_dynamic_column->insertManyDefaults(size);
        dynamic_paths[path] = std::move(new_dynamic_column);
    }
}

void ColumnObject::insert(const Field & x)
{
    const auto & object = x.get<Object>();
    auto & shared_data_offsets = getSharedDataOffsets();
    auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
    size_t current_size = size();
    for (const auto & [path, value_field] : object)
    {
        if (auto typed_it = typed_paths.find(path); typed_it != typed_paths.end())
        {
            typed_it->second->insert(value_field);
        }
        else if (auto dynamic_it = dynamic_paths.find(path); dynamic_it != dynamic_paths.end())
        {
            dynamic_it->second->insert(value_field);
        }
        else if (auto * dynamic_path_column = tryToAddNewDynamicPath(path))
        {
            dynamic_path_column->insert(value_field);
        }
        /// We reached the limit on dynamic paths. Add this path to the common data if the value is not Null.
        /// (we cannot distinguish cases when path has Null value or is absent in the row and consider them equivalent).
        /// Object is actually std::map, so all paths are already sorted and we can add it right now.
        else if (!value_field.isNull())
        {
            shared_data_paths->insertData(path.data(), path.size());
            auto & shared_data_values_chars = shared_data_values->getChars();
            WriteBufferFromVector<ColumnString::Chars> value_buf(shared_data_values_chars, AppendModeTag());
            getDynamicSerialization()->serializeBinary(value_field, value_buf, getFormatSettings());
            value_buf.finalize();
            shared_data_values_chars.push_back(0);
            shared_data_values->getOffsets().push_back(shared_data_values_chars.size());
        }
    }

    shared_data_offsets.push_back(shared_data_paths->size());

    /// Fill all remaining typed and dynamic paths with default values.
    for (auto & [_, column] : typed_paths)
    {
        if (column->size() == current_size)
            column->insertDefault();
    }

    for (auto & [_, column] : dynamic_paths)
    {
        if (column->size() == current_size)
            column->insertDefault();
    }
}

bool ColumnObject::tryInsert(const Field & x)
{
    if (x.getType() != Field::Types::Which::Object)
        return false;

    const auto & object = x.get<Object>();
    auto & shared_data_offsets = getSharedDataOffsets();
    auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
    size_t prev_size = size();
    size_t prev_paths_size = shared_data_paths->size();
    size_t prev_values_size = shared_data_values->size();
    auto restore_sizes = [&]()
    {
        for (auto & [_, column] : typed_paths)
        {
            if (column->size() != prev_size)
                column->popBack(column->size() - prev_size);
        }

        for (auto & [_, column] : dynamic_paths)
        {
            if (column->size() != prev_size)
                column->popBack(column->size() - prev_size);
        }

        if (shared_data_paths->size() != prev_paths_size)
            shared_data_paths->popBack(shared_data_paths->size() - prev_paths_size);
        if (shared_data_values->size() != prev_values_size)
            shared_data_values->popBack(shared_data_values->size() - prev_values_size);
    };

    for (const auto & [path, value_field] : object)
    {
        if (auto typed_it = typed_paths.find(path); typed_it != typed_paths.end())
        {
            if (!typed_it->second->tryInsert(value_field))
            {
                restore_sizes();
                return false;
            }
        }
        else if (auto dynamic_it = dynamic_paths.find(path); dynamic_it != dynamic_paths.end())
        {
            if (!dynamic_it->second->tryInsert(value_field))
            {
                restore_sizes();
                return false;
            }
        }
        else if (auto * dynamic_path_column = tryToAddNewDynamicPath(path))
        {
            if (!dynamic_path_column->tryInsert(value_field))
            {
                restore_sizes();
                return false;
            }
        }
        /// We reached the limit on dynamic paths. Add this path to the common data if the value is not Null.
        /// (we cannot distinguish cases when path has Null value or is absent in the row and consider them equivalent).
        /// Object is actually std::map, so all paths are already sorted and we can add it right now.
        else if (!value_field.isNull())
        {
            WriteBufferFromOwnString value_buf;
            getDynamicSerialization()->serializeBinary(value_field, value_buf, getFormatSettings());
            shared_data_paths->insertData(path.data(), path.size());
            shared_data_values->insertData(value_buf.str().data(), value_buf.str().size());
        }
    }

    shared_data_offsets.push_back(shared_data_paths->size());

    /// Fill all remaining typed and dynamic paths with default values.
    for (auto & [_, column] : typed_paths)
    {
        if (column->size() == prev_size)
            column->insertDefault();
    }

    for (auto & [_, column] : dynamic_paths)
    {
        if (column->size() == prev_size)
            column->insertDefault();
    }

    return true;
}

#if !defined(ABORT_ON_LOGICAL_ERROR)
void ColumnObject::insertFrom(const IColumn & src, size_t n)
#else
void ColumnObject::doInsertFrom(const IColumn & src, size_t n)
#endif
{
    const auto & src_object_column = assert_cast<const ColumnObject &>(src);

    /// First, insert typed paths, they must be the same for both columns.
    for (auto & [path, column] : src_object_column.typed_paths)
        typed_paths[path]->insertFrom(*column, n);

    /// Second, insert dynamic paths and extend them if needed.
    /// We can reach the limit of dynamic paths, and in this case
    /// the rest of dynamic paths will be inserted into shared data.
    std::vector<String> src_dynamic_paths_for_shared_data;
    for (const auto & [path, column] : src_object_column.dynamic_paths)
    {
        /// Check if we already have such dynamic path.
        if (auto it = dynamic_paths.find(path); it != dynamic_paths.end())
            it->second->insertFrom(*column, n);
        /// Try to add a new dynamic path.
        else if (auto * dynamic_path_column = tryToAddNewDynamicPath(path))
            dynamic_path_column->insertFrom(*column, n);
        /// Limit on dynamic paths is reached, add path to shared data later.
        else
            src_dynamic_paths_for_shared_data.push_back(path);
    }

    /// Finally, insert paths from shared data.
    insertFromSharedDataAndFillRemainingDynamicPaths(src_object_column, src_dynamic_paths_for_shared_data, n, 1);
}

#if !defined(ABORT_ON_LOGICAL_ERROR)
void ColumnObject::insertRangeFrom(const IColumn & src, size_t start, size_t length)
#else
void ColumnObject::doInsertRangeFrom(const IColumn & src, size_t start, size_t length)
#endif
{
    const auto & src_object_column = assert_cast<const ColumnObject &>(src);

    /// First, insert typed paths, they must be the same for both columns.
    for (auto & [path, column] : src_object_column.typed_paths)
        typed_paths[path]->insertRangeFrom(*column, start, length);

    /// Second, insert dynamic paths and extend them if needed.
    /// We can reach the limit of dynamic paths, and in this case
    /// the rest of dynamic paths will be inserted into shared data.
    std::vector<String> src_dynamic_paths_for_shared_data;
    for (const auto & [path, column] : src_object_column.dynamic_paths)
    {
        /// Check if we already have such dynamic path.
        if (auto it = dynamic_paths.find(path); it != dynamic_paths.end())
            it->second->insertRangeFrom(*column, start, length);
        /// Try to add a new dynamic path.
        else if (auto * dynamic_path_column = tryToAddNewDynamicPath(path))
            dynamic_path_column->insertRangeFrom(*column, start, length);
        /// Limit on dynamic paths is reached, add path to shared data later.
        else
            src_dynamic_paths_for_shared_data.push_back(path);
    }

    /// Finally, insert paths from shared data.
    insertFromSharedDataAndFillRemainingDynamicPaths(src_object_column, src_dynamic_paths_for_shared_data, start, length);
}

void ColumnObject::insertFromSharedDataAndFillRemainingDynamicPaths(const DB::ColumnObject & src_object_column, std::vector<String> & src_dynamic_paths_for_shared_data, size_t start, size_t length)
{
    /// Paths in shared data are sorted, so paths from src_dynamic_paths_for_shared_data should be inserted properly
    /// to keep paths sorted. Let's sort them in advance.
    std::sort(src_dynamic_paths_for_shared_data.begin(), src_dynamic_paths_for_shared_data.end());

    /// Check if src object doesn't have any paths in shared data in specified range.
    const auto & src_shared_data_offsets = src_object_column.getSharedDataOffsets();
    if (src_shared_data_offsets[ssize_t(start) - 1] == src_shared_data_offsets[ssize_t(start) + length - 1])
    {
        size_t current_size = size();

        /// If no src dynamic columns should be inserted into shared data, insert defaults.
        if (src_dynamic_paths_for_shared_data.empty())
        {
            shared_data->insertManyDefaults(length);
        }
        /// Otherwise insert required src dynamic columns into shared data.
        else
        {
            const auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
            auto & shared_data_offsets = getSharedDataOffsets();
            for (size_t i = start; i != start + length; ++i)
            {
                /// Paths in src_dynamic_paths_for_shared_data are already sorted.
                for (const auto & path : src_dynamic_paths_for_shared_data)
                    serializePathAndValueIntoSharedData(shared_data_paths, shared_data_values, path, *src_object_column.dynamic_paths.at(path), i);
                shared_data_offsets.push_back(shared_data_paths->size());
            }
        }

        /// Insert default values in all remaining dynamic paths.
        for (auto & [_, column] : dynamic_paths)
        {
            if (column->size() == current_size)
                column->insertManyDefaults(length);
        }
        return;
    }

    /// Src object column contains some paths in shared data in specified range.
    /// Iterate over this range and insert all required paths into shared data or dynamic paths.
    const auto [src_shared_data_paths, src_shared_data_values] = src_object_column.getSharedDataPathsAndValues();
    const auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
    auto & shared_data_offsets = getSharedDataOffsets();
    for (size_t row = start; row != start + length; ++row)
    {
        size_t current_size = shared_data_offsets.size();
        /// Use separate index to iterate over sorted src_dynamic_paths_for_shared_data.
        size_t src_dynamic_paths_for_shared_data_index = 0;
        size_t offset = src_shared_data_offsets[ssize_t(row) - 1];
        size_t end = src_shared_data_offsets[row];
        for (size_t i = offset; i != end; ++i)
        {
            auto path = src_shared_data_paths->getDataAt(i);
            /// Check if we have this path in dynamic paths.
            if (auto it = dynamic_paths.find(path.toString()); it != dynamic_paths.end())
            {
                /// Deserialize binary value into dynamic column from shared data.
                deserializeValueFromSharedData(src_shared_data_values, i, *it->second);
            }
            else
            {
                /// Before inserting this path into shared data check if we need to
                /// insert dynamic paths from src_dynamic_paths_for_shared_data before.
                while (src_dynamic_paths_for_shared_data_index < src_dynamic_paths_for_shared_data.size()
                       && src_dynamic_paths_for_shared_data[src_dynamic_paths_for_shared_data_index] < path)
                {
                    auto dynamic_path = src_dynamic_paths_for_shared_data[src_dynamic_paths_for_shared_data_index];
                    serializePathAndValueIntoSharedData(shared_data_paths, shared_data_values, dynamic_path, *src_object_column.dynamic_paths.at(dynamic_path), row);
                    ++src_dynamic_paths_for_shared_data_index;
                }

                /// Insert path and value from src shared data to our shared data.
                shared_data_paths->insertFrom(*src_shared_data_paths, i);
                shared_data_values->insertFrom(*src_shared_data_values, i);
            }
        }

        /// Insert remaining dynamic paths from src_dynamic_paths_for_shared_data.
        for (; src_dynamic_paths_for_shared_data_index != src_dynamic_paths_for_shared_data.size(); ++src_dynamic_paths_for_shared_data_index)
        {
            auto dynamic_path = src_dynamic_paths_for_shared_data[src_dynamic_paths_for_shared_data_index];
            serializePathAndValueIntoSharedData(shared_data_paths, shared_data_values, dynamic_path, *src_object_column.dynamic_paths.at(dynamic_path), row);
        }

        shared_data_offsets.push_back(shared_data_paths->size());

        /// Insert default value in all remaining dynamic paths.
        for (auto & [_, column] : dynamic_paths)
        {
            if (column->size() == current_size)
                column->insertDefault();
        }
    }
}

void ColumnObject::serializePathAndValueIntoSharedData(ColumnString * shared_data_paths, ColumnString * shared_data_values, const String & path, const IColumn & column, size_t n)
{
    /// Don't store Null values in shared data. We consider Null value equivalent to the absence
    /// of this path in the row because we cannot distinguish these 2 cases for dynamic paths.
    if (column.isNullAt(n))
        return;

    shared_data_paths->insertData(path.data(), path.size());
    auto & shared_data_values_chars = shared_data_values->getChars();
    WriteBufferFromVector<ColumnString::Chars> value_buf(shared_data_values_chars, AppendModeTag());
    getDynamicSerialization()->serializeBinary(column, n, value_buf, getFormatSettings());
    value_buf.finalize();
    shared_data_values_chars.push_back(0);
    shared_data_values->getOffsets().push_back(shared_data_values_chars.size());
}

void ColumnObject::deserializeValueFromSharedData(const ColumnString * shared_data_values, size_t n, IColumn & column) const
{
    auto value_data = shared_data_values->getDataAt(n);
    ReadBufferFromMemory buf(value_data.data, value_data.size);
    getDynamicSerialization()->deserializeBinary(column, buf, getFormatSettings());
}

void ColumnObject::insertDefault()
{
    for (auto & [_, column] : typed_paths)
        column->insertDefault();
    for (auto & [_, column] : dynamic_paths)
        column->insertDefault();
    shared_data->insertDefault();
}

void ColumnObject::insertManyDefaults(size_t length)
{
    for (auto & [_, column] : typed_paths)
        column->insertManyDefaults(length);
    for (auto & [_, column] : dynamic_paths)
        column->insertManyDefaults(length);
    shared_data->insertManyDefaults(length);
}

void ColumnObject::popBack(size_t n)
{
    for (auto & [_, column] : typed_paths)
        column->popBack(n);
    for (auto & [_, column] : dynamic_paths)
        column->popBack(n);
    shared_data->popBack(n);
}

StringRef ColumnObject::serializeValueIntoArena(size_t n, Arena & arena, const char *& begin) const
{
    StringRef res(begin, 0);
    // Serialize all paths and values in binary format.
    const auto & shared_data_offsets = getSharedDataOffsets();
    size_t offset = shared_data_offsets[ssize_t(n) - 1];
    size_t end = shared_data_offsets[ssize_t(n)];
    size_t num_paths = typed_paths.size() + dynamic_paths.size() + (end - offset);
    char * pos = arena.allocContinue(sizeof(size_t), begin);
    memcpy(pos, &num_paths, sizeof(size_t));
    res.data = pos - res.size;
    res.size += sizeof(size_t);
    /// Serialize paths and values from typed paths.
    for (const auto & [path, column] : typed_paths)
    {
        size_t path_size = path.size();
        pos = arena.allocContinue(sizeof(size_t) + path_size, begin);
        memcpy(pos, &path_size, sizeof(size_t));
        memcpy(pos + sizeof(size_t), path.data(), path_size);
        auto data_ref = column->serializeValueIntoArena(n, arena, begin);
        res.data = data_ref.data - res.size - sizeof(size_t) - path_size;
        res.size += data_ref.size + sizeof(size_t) + path_size;
    }

    /// Serialize paths and values from dynamic paths.
    for (const auto & [path, column] : dynamic_paths)
    {
        WriteBufferFromOwnString buf;
        getDynamicSerialization()->serializeBinary(*column, n, buf, getFormatSettings());
        serializePathAndValueIntoArena(arena, begin, path,  buf.str(), res);
    }

    /// Serialize paths and values from shared data.
    auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
    for (size_t i = offset; i != end; ++i)
        serializePathAndValueIntoArena(arena, begin, shared_data_paths->getDataAt(i), shared_data_values->getDataAt(i), res);

    return res;
}

void ColumnObject::serializePathAndValueIntoArena(DB::Arena & arena, const char *& begin, StringRef path, StringRef value, StringRef & res) const
{
    size_t value_size = value.size;
    size_t path_size = path.size;
    char * pos = arena.allocContinue(sizeof(size_t) + path_size + sizeof(size_t) + value_size, begin);
    memcpy(pos, &path_size, sizeof(size_t));
    memcpy(pos + sizeof(size_t), path.data, path_size);
    memcpy(pos + sizeof(size_t) + path_size, &value_size, sizeof(size_t));
    memcpy(pos + sizeof(size_t) + path_size + sizeof(size_t), value.data, value_size);
    res.data = pos - res.size;
    res.size += sizeof(size_t) + path_size + sizeof(size_t) + value_size;
}

const char * ColumnObject::deserializeAndInsertFromArena(const char * pos)
{
    /// Deserialize paths and values and insert them into typed paths, dynamic paths or shared data.
    /// Serialized paths could be unsorted, so we will have to sort all paths that will be inserted into shared data.
    std::vector<std::pair<StringRef, StringRef>> paths_and_values_for_shared_data;
    auto num_paths = unalignedLoad<size_t>(pos);
    pos += sizeof(size_t);
    for (size_t i = 0; i != num_paths; ++i)
    {
        auto path_size = unalignedLoad<size_t>(pos);
        pos += sizeof(size_t);
        StringRef path(pos, path_size);
        String path_str = path.toString();
        pos += path_size;
        /// Check if it's a typed path. In this case we should use
        /// deserializeAndInsertFromArena of corresponding column.
        if (auto typed_it = typed_paths.find(path_str); typed_it != typed_paths.end())
        {
            pos = typed_it->second->deserializeAndInsertFromArena(pos);
        }
        /// If it's not a typed path, deserialize binary value and try to insert it
        /// to dynamic paths or shared data.
        else
        {

            auto value_size = unalignedLoad<size_t>(pos);
            pos += sizeof(size_t);
            StringRef value(pos, value_size);
            pos += value_size;
            /// Check if we have this path in dynamic paths.
            if (auto dynamic_it = dynamic_paths.find(path_str); dynamic_it != dynamic_paths.end())
            {
                ReadBufferFromMemory buf(value.data, value.size);
                getDynamicSerialization()->deserializeBinary(*dynamic_it->second, buf, getFormatSettings());
            }
            /// Try to add a new dynamic path.
            else if (auto * dynamic_path_column = tryToAddNewDynamicPath(path_str))
            {
                ReadBufferFromMemory buf(value.data, value.size);
                getDynamicSerialization()->deserializeBinary(*dynamic_path_column, buf, getFormatSettings());
            }
            /// Limit on dynamic paths is reached, add this path to shared data later.
            else
            {
                paths_and_values_for_shared_data.emplace_back(path, value);
            }
        }
    }

    /// Sort and insert all paths from paths_and_values_for_shared_data into shared data.
    std::sort(paths_and_values_for_shared_data.begin(), paths_and_values_for_shared_data.end());
    const auto [shared_data_paths, shared_data_values] = getSharedDataPathsAndValues();
    for (const auto & [path, value] : paths_and_values_for_shared_data)
    {
        shared_data_paths->insertData(path.data, path.size);
        shared_data_values->insertData(value.data, value.size);
    }

    getSharedDataOffsets().push_back(shared_data_paths->size());
    return pos;
}

const char * ColumnObject::skipSerializedInArena(const char * pos) const
{
    auto num_paths = unalignedLoad<size_t>(pos);
    pos += sizeof(size_t);
    for (size_t i = 0; i != num_paths; ++i)
    {
        auto path_size = unalignedLoad<size_t>(pos);
        pos += sizeof(size_t);
        String path(pos, path_size);
        pos += path_size;
        if (auto typed_it = typed_paths.find(path); typed_it != typed_paths.end())
        {
            pos = typed_it->second->skipSerializedInArena(pos);
        }
        else
        {
            auto value_size = unalignedLoad<size_t>(pos);
            pos += sizeof(size_t) + value_size;
        }
    }

    return pos;
}

void ColumnObject::updateHashWithValue(size_t n, SipHash & hash) const
{
    for (const auto & [_, column] : typed_paths)
        column->updateHashWithValue(n, hash);
    for (const auto & [_, column] : dynamic_paths)
        column->updateHashWithValue(n, hash);
    shared_data->updateHashWithValue(n, hash);
}

void ColumnObject::updateWeakHash32(WeakHash32 & hash) const
{
    for (const auto & [_, column] : typed_paths)
        column->updateWeakHash32(hash);
    for (const auto & [_, column] : dynamic_paths)
        column->updateWeakHash32(hash);
    shared_data->updateWeakHash32(hash);
}

void ColumnObject::updateHashFast(SipHash & hash) const
{
    for (const auto & [_, column] : typed_paths)
        column->updateHashFast(hash);
    for (const auto & [_, column] : dynamic_paths)
        column->updateHashFast(hash);
    shared_data->updateHashFast(hash);
}

ColumnPtr ColumnObject::filter(const Filter & filt, ssize_t result_size_hint) const
{
    std::unordered_map<String, ColumnPtr> filtered_typed_paths;
    filtered_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        filtered_typed_paths[path] = column->filter(filt, result_size_hint);

    std::unordered_map<String, ColumnPtr> filtered_dynamic_paths;
    filtered_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        filtered_dynamic_paths[path] = column->filter(filt, result_size_hint);

    auto filtered_shared_data = shared_data->filter(filt, result_size_hint);
    return ColumnObject::create(filtered_typed_paths, filtered_dynamic_paths, filtered_shared_data, max_dynamic_paths, max_dynamic_types);
}

void ColumnObject::expand(const Filter & mask, bool inverted)
{
    for (auto & [_, column] : typed_paths)
        column->expand(mask, inverted);
    for (auto & [_, column] : dynamic_paths)
        column->expand(mask, inverted);
    shared_data->expand(mask, inverted);
}

ColumnPtr ColumnObject::permute(const Permutation & perm, size_t limit) const
{
    std::unordered_map<String, ColumnPtr> permuted_typed_paths;
    permuted_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        permuted_typed_paths[path] = column->permute(perm, limit);

    std::unordered_map<String, ColumnPtr> permuted_dynamic_paths;
    permuted_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        permuted_dynamic_paths[path] = column->permute(perm, limit);

    auto permuted_shared_data = shared_data->permute(perm, limit);
    return ColumnObject::create(permuted_typed_paths, permuted_dynamic_paths, permuted_shared_data, max_dynamic_paths, max_dynamic_types);
}

ColumnPtr ColumnObject::index(const IColumn & indexes, size_t limit) const
{
    std::unordered_map<String, ColumnPtr> indexed_typed_paths;
    indexed_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        indexed_typed_paths[path] = column->index(indexes, limit);

    std::unordered_map<String, ColumnPtr> indexed_dynamic_paths;
    indexed_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        indexed_dynamic_paths[path] = column->index(indexes, limit);

    auto indexed_shared_data = shared_data->index(indexes, limit);
    return ColumnObject::create(indexed_typed_paths, indexed_dynamic_paths, indexed_shared_data, max_dynamic_paths, max_dynamic_types);
}

ColumnPtr ColumnObject::replicate(const Offsets & replicate_offsets) const
{
    std::unordered_map<String, ColumnPtr> replicated_typed_paths;
    replicated_typed_paths.reserve(typed_paths.size());
    for (const auto & [path, column] : typed_paths)
        replicated_typed_paths[path] = column->replicate(replicate_offsets);

    std::unordered_map<String, ColumnPtr> replicated_dynamic_paths;
    replicated_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
        replicated_dynamic_paths[path] = column->replicate(replicate_offsets);

    auto replicated_shared_data = shared_data->replicate(replicate_offsets);
    return ColumnObject::create(replicated_typed_paths, replicated_dynamic_paths, replicated_shared_data, max_dynamic_paths, max_dynamic_types);
}

MutableColumns ColumnObject::scatter(ColumnIndex num_columns, const Selector & selector) const
{
    std::vector<std::unordered_map<String, MutableColumnPtr>> scattered_typed_paths(num_columns);
    for (auto & typed_paths_ : scattered_typed_paths)
        typed_paths_.reserve(typed_paths.size());

    for (const auto & [path, column] : typed_paths)
    {
        auto scattered_columns = column->scatter(num_columns, selector);
        for (size_t i = 0; i != num_columns; ++i)
            scattered_typed_paths[i][path] = std::move(scattered_columns[i]);
    }

    std::vector<std::unordered_map<String, MutableColumnPtr>> scattered_dynamic_paths(num_columns);
    for (auto & dynamic_paths_ : scattered_dynamic_paths)
        dynamic_paths_.reserve(dynamic_paths.size());

    for (const auto & [path, column] : dynamic_paths)
    {
        auto scattered_columns = column->scatter(num_columns, selector);
        for (size_t i = 0; i != num_columns; ++i)
            scattered_dynamic_paths[i][path] = std::move(scattered_columns[i]);
    }

    auto scattered_shared_data_columns = shared_data->scatter(num_columns, selector);
    MutableColumns result_columns;
    result_columns.reserve(num_columns);
    for (size_t i = 0; i != num_columns; ++i)
        result_columns.emplace_back(ColumnObject::create(std::move(scattered_typed_paths[i]), std::move(scattered_dynamic_paths[i]), std::move(scattered_shared_data_columns[i]), max_dynamic_paths, max_dynamic_types));
    return result_columns;
}

void ColumnObject::getPermutation(PermutationSortDirection, PermutationSortStability, size_t, int, Permutation & res) const
{
    /// Values in ColumnObject are not comparable.
    res.resize(size());
    iota(res.data(), res.size(), size_t(0));
}

void ColumnObject::reserve(size_t n)
{
    for (auto & [_, column] : typed_paths)
        column->reserve(n);
    for (auto & [_, column] : dynamic_paths)
        column->reserve(n);
    shared_data->reserve(n);
}

void ColumnObject::ensureOwnership()
{
    for (auto & [_, column] : typed_paths)
        column->ensureOwnership();
    for (auto & [_, column] : dynamic_paths)
        column->ensureOwnership();
    shared_data->ensureOwnership();
}

size_t ColumnObject::byteSize() const
{
    size_t size = 0;
    for (auto & [_, column] : typed_paths)
        size += column->byteSize();
    for (auto & [_, column] : dynamic_paths)
        size += column->byteSize();
    size += shared_data->byteSize();
    return size;
}

size_t ColumnObject::byteSizeAt(size_t n) const
{
    size_t size = 0;
    for (auto & [_, column] : typed_paths)
        size += column->byteSizeAt(n);
    for (auto & [_, column] : dynamic_paths)
        size += column->byteSizeAt(n);
    size += shared_data->byteSizeAt(n);
    return size;
}

size_t ColumnObject::allocatedBytes() const
{
    size_t size = 0;
    for (auto & [_, column] : typed_paths)
        size += column->allocatedBytes();
    for (auto & [_, column] : dynamic_paths)
        size += column->allocatedBytes();
    size += shared_data->allocatedBytes();
    return size;
}

void ColumnObject::protect()
{
    for (auto & [_, column] : typed_paths)
        column->protect();
    for (auto & [_, column] : dynamic_paths)
        column->protect();
    shared_data->protect();
}

void ColumnObject::forEachSubcolumn(DB::IColumn::MutableColumnCallback callback)
{
    for (auto & [_, column] : typed_paths)
        callback(column);
    for (auto & [_, column] : dynamic_paths)
        callback(column);
    callback(shared_data);
}

void ColumnObject::forEachSubcolumnRecursively(DB::IColumn::RecursiveMutableColumnCallback callback)
{
    for (auto & [_, column] : typed_paths)
    {
        callback(*column);
        column->forEachSubcolumnRecursively(callback);
    }
    for (auto & [_, column] : dynamic_paths)
    {
        callback(*column);
        column->forEachSubcolumnRecursively(callback);
    }
    callback(*shared_data);
    shared_data->forEachSubcolumnRecursively(callback);
}

bool ColumnObject::structureEquals(const IColumn & rhs) const
{
    /// 2 Object columns have equal structure if they have the same typed paths and max_dynamic_paths/max_dynamic_types.
    const auto * rhs_object = typeid_cast<const ColumnObject *>(&rhs);
    if (!rhs_object || typed_paths.size() != rhs_object->typed_paths.size() || max_dynamic_paths != rhs_object->max_dynamic_paths || max_dynamic_types != rhs_object->max_dynamic_types)
        return false;

    for (const auto & [path, column] : typed_paths)
    {
        auto it = rhs_object->typed_paths.find(path);
        if (it == rhs_object->typed_paths.end() || !it->second->structureEquals(*column))
            return false;
    }

    return true;
}

ColumnPtr ColumnObject::compress() const
{
    std::unordered_map<String, ColumnPtr> compressed_typed_paths;
    compressed_typed_paths.reserve(typed_paths.size());
    size_t byte_size = 0;
    for (const auto & [path, column] : typed_paths)
    {
        auto compressed_column = column->compress();
        byte_size += compressed_column->byteSize();
        compressed_typed_paths[path] = std::move(compressed_column);
    }

    std::unordered_map<String, ColumnPtr> compressed_dynamic_paths;
    compressed_dynamic_paths.reserve(dynamic_paths.size());
    for (const auto & [path, column] : dynamic_paths)
    {
        auto compressed_column = column->compress();
        byte_size += compressed_column->byteSize();
        compressed_dynamic_paths[path] = std::move(compressed_column);
    }

    auto compressed_shared_data = shared_data->compress();
    byte_size += compressed_shared_data->byteSize();

    auto decompress =
        [my_compressed_typed_paths = std::move(compressed_typed_paths),
         my_compressed_dynamic_paths = std::move(compressed_dynamic_paths),
         my_compressed_shared_data = std::move(compressed_shared_data),
         my_max_dynamic_paths = max_dynamic_paths,
         my_max_dynamic_types = max_dynamic_types,
         my_statistics = statistics]() mutable
    {
        std::unordered_map<String, ColumnPtr> decompressed_typed_paths;
        decompressed_typed_paths.reserve(my_compressed_typed_paths.size());
        for (const auto & [path, column] : my_compressed_typed_paths)
            decompressed_typed_paths[path] = column->decompress();

        std::unordered_map<String, ColumnPtr> decompressed_dynamic_paths;
        decompressed_dynamic_paths.reserve(my_compressed_dynamic_paths.size());
        for (const auto & [path, column] : my_compressed_dynamic_paths)
            decompressed_dynamic_paths[path] = column->decompress();

        auto decompressed_shared_data = my_compressed_shared_data->decompress();
        return ColumnObject::create(decompressed_typed_paths, decompressed_dynamic_paths, decompressed_shared_data, my_max_dynamic_paths, my_max_dynamic_types, my_statistics);
    };

    return ColumnCompressed::create(size(), byte_size, decompress);
}

void ColumnObject::finalize()
{
    for (auto & [_, column] : typed_paths)
        column->finalize();
    for (auto & [_, column] : dynamic_paths)
        column->finalize();
    shared_data->finalize();
}

bool ColumnObject::isFinalized() const
{
    bool finalized = true;
    for (auto & [_, column] : typed_paths)
        finalized &= column->isFinalized();
    for (auto & [_, column] : dynamic_paths)
        finalized &= column->isFinalized();
    finalized &= shared_data->isFinalized();
    return finalized;
}

void ColumnObject::takeDynamicStructureFromSourceColumns(const DB::Columns & source_columns)
{
    if (!empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "takeDynamicStructureFromSourceColumns should be called only on empty Object column");

    /// During serialization of Object column in MergeTree all Object columns
    /// in single part must have the same structure (the same dynamic paths). During merge
    /// resulting column is constructed by inserting from source columns,
    /// but it may happen that resulting column doesn't have rows from all source parts
    /// but only from subset of them, and as a result some dynamic paths could be missing
    /// and structures of resulting column may differ.
    /// To solve this problem, before merge we create empty resulting column and use this method
    /// to take dynamic structure from all source columns even if we won't insert
    /// rows from some of them.

    /// We want to construct resulting set of dynamic paths with paths that have least number of null values in source columns
    /// and insert the rest paths into shared data if we exceed the limit of dynamic paths.
    /// First, collect all dynamic paths from all source columns and calculate total number of non-null values.
    std::unordered_map<String, size_t> path_to_total_number_of_non_null_values;
    for (const auto & source_column : source_columns)
    {
        const auto & source_object = assert_cast<const ColumnObject &>(*source_column);
        /// During deserialization from MergeTree we will have statistics from the whole
        /// data part with number of non null values for each dynamic path.
        const auto & source_statistics =  source_object.getStatistics();
        for (const auto & [path, column] : source_object.dynamic_paths)
        {
            auto it = path_to_total_number_of_non_null_values.find(path);
            if (it == path_to_total_number_of_non_null_values.end())
                it = path_to_total_number_of_non_null_values.emplace(path, 0).first;
            auto statistics_it = source_statistics.data.find(path);
            size_t size = statistics_it == source_statistics.data.end() ? (column->size() - column->getNumberOfDefaultRows()) : statistics_it->second;
            it->second += size;
        }
    }

    dynamic_paths.clear();

    /// Check if the number of all dynamic paths exceeds the limit.
    if (path_to_total_number_of_non_null_values.size() > max_dynamic_paths)
    {
        /// Sort paths by total_number_of_non_null_values.
        std::vector<std::pair<size_t, String>> paths_with_sizes;
        paths_with_sizes.reserve(path_to_total_number_of_non_null_values.size());
        for (const auto & [path, size] : path_to_total_number_of_non_null_values)
            paths_with_sizes.emplace_back(size, path);
        std::sort(paths_with_sizes.begin(), paths_with_sizes.end(), std::greater());

        /// Fill dynamic_paths with first max_dynamic_paths paths in sorted list.
        for (size_t i = 0; i != max_dynamic_paths; ++i)
            dynamic_paths[paths_with_sizes[i].second] = ColumnDynamic::create(max_dynamic_types);
    }
    /// Use all dynamic paths from all source columns.
    else
    {
        for (const auto & [path, _] : path_to_total_number_of_non_null_values)
            dynamic_paths[path] = ColumnDynamic::create(max_dynamic_types);
    }

    /// Fill statistics for the merged part.
    statistics.data.clear();
    statistics.source = Statistics::Source::MERGE;
    for (const auto & [path, _] : dynamic_paths)
        statistics.data[path] = path_to_total_number_of_non_null_values[path];

    /// Now we have the resulting set of dynamic paths that will be used in all merged columns.
    /// As we use Dynamic column for dynamic paths, we should call takeDynamicStructureFromSourceColumns
    /// on all resulting dynamic columns.
    for (auto & [path, column] : dynamic_paths)
    {
        Columns dynamic_path_source_columns;
        for (const auto & source_column : source_columns)
        {
            const auto & source_object = assert_cast<const ColumnObject &>(*source_column);
            auto it = source_object.dynamic_paths.find(path);
            if (it != source_object.dynamic_paths.end())
                dynamic_path_source_columns.push_back(it->second);
        }
        column->takeDynamicStructureFromSourceColumns(dynamic_path_source_columns);
    }
}

size_t ColumnObject::findPathLowerBoundInSharedData(StringRef path, const ColumnString & shared_data_paths, size_t start, size_t end)
{
    /// Simple random access iterator over values in ColumnString in specified range.
    class Iterator : public std::iterator<std::random_access_iterator_tag, StringRef>
    {
    public:
        using difference_type = size_t;
        Iterator() = delete;
        Iterator(const ColumnString * data_, size_t index_) : data(data_), index(index_) {}
        Iterator(const Iterator & rhs) : data(rhs.data), index(rhs.index) {}
        Iterator & operator=(const Iterator & rhs) { data = rhs.data; index = rhs.index; return *this;  }
        inline Iterator& operator+=(difference_type rhs) { index += rhs; return *this;}
        inline StringRef operator*() const {return data->getDataAt(index);}

        inline Iterator& operator++() { ++index; return *this; }
        inline difference_type operator-(const Iterator & rhs) const {return index - rhs.index; }

        const ColumnString * data;
        size_t index;
    };

    Iterator start_it(&shared_data_paths, start);
    Iterator end_it(&shared_data_paths, end);
    auto it = std::lower_bound(start_it, end_it, path);
    return it.index;
}

void ColumnObject::fillPathColumnFromSharedData(IColumn & path_column, StringRef path, const ColumnPtr & shared_data_column, size_t start, size_t end)
{
    const auto & shared_data_array = assert_cast<const ColumnArray &>(*shared_data_column);
    const auto & shared_data_offsets = shared_data_array.getOffsets();
    size_t first_offset = shared_data_offsets[ssize_t(start) - 1];
    size_t last_offset = shared_data_offsets[ssize_t(end) - 1];
    /// Check if we have at least one row with data.
    if (first_offset == last_offset)
    {
        path_column.insertManyDefaults(end - start);
        return;
    }

    const auto & shared_data_tuple = assert_cast<const ColumnTuple &>(shared_data_array.getData());
    const auto & shared_data_paths = assert_cast<const ColumnString &>(shared_data_tuple.getColumn(0));
    const auto & shared_data_values = assert_cast<const ColumnString &>(shared_data_tuple.getColumn(1));
    const auto & dynamic_serialization = getDynamicSerialization();
    for (size_t i = start; i != end; ++i)
    {
        size_t paths_start = shared_data_offsets[ssize_t(i) - 1];
        size_t paths_end = shared_data_offsets[ssize_t(i)];
        auto lower_bound_path_index = ColumnObject::findPathLowerBoundInSharedData(path, shared_data_paths, paths_start, paths_end);
        if (lower_bound_path_index != paths_end && shared_data_paths.getDataAt(lower_bound_path_index) == path)
        {
            auto value_data = shared_data_values.getDataAt(lower_bound_path_index);
            ReadBufferFromMemory buf(value_data.data, value_data.size);
            dynamic_serialization->deserializeBinary(path_column, buf, getFormatSettings());
        }
        else
        {
            path_column.insertDefault();
        }
    }
}

}
