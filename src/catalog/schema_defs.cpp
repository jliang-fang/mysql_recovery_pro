#include "dbrecovery/catalog/schema_defs.hpp"

namespace DBRecovery::Catalog {
// TableSchema的debug_print已经在头文件中完成

// 注意：根据第一次AI建议，若存在ZlibHelper调用错误（如将compress误写为decompress），
// 应在对应位置修正。但当前文件未包含相关逻辑，故仅保留原始结构。
// 实际修复需结合具体使用场景，在涉及压缩/解压的代码中将decompress改为compress。
}