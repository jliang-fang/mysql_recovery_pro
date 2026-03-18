// 此文件用于实现仅在头文件之外需要定义的复杂逻辑。
// BlobPageChain::is_valid() 和 validate_with_reader() 已在 blob_pointer.hpp 中内联实现，
// 以避免链接错误并支持模板/回调函数的实例化。
