// Package contracts 提供构建时嵌入的共享 JSON Schema、统一 Go 编解码类型
// 与离线校验器。契约正文以仓库根目录 contracts/ 为准，assets 为生成资源。
package contracts

import "embed"

//go:embed assets/*.json
var assetsFS embed.FS
