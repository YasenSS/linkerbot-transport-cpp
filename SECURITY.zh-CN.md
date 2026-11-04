# 支持范围与敏感报告

[English](SECURITY.md) · [简体中文](SECURITY.zh-CN.md)

已发布稳定版见[发行页面](https://github.com/linker-bot/linkerbot-transport-cpp/releases/latest)。
维护以当前 `main` 分支为准，尽力处理问题。发布稳定版不代表另设长期支持分支或承诺响应时限；
历史提交和未发布的 CI 产物没有单独的支持承诺。

## 普通项目问题

构建失败、配置错误、生命周期缺陷、进程退出崩溃、驱动错误和硬件兼容缺口通常属于可靠性问题。
报告应提供固定提交、触发条件、前置条件、所需权限、实际影响及不受影响的范围。
只有确实跨越安全边界时，才应将问题描述为认证绕过或漏洞。

非敏感报告可提交到[主项目的问题跟踪器](https://github.com/linker-bot/linkerbot-transport-cpp/issues)
（启用时），或提交带最小复现的 Pull Request。遵循[贡献指南](CONTRIBUTING.zh-CN.md)，
移除凭据、验证码、私有日志、专有头文件、厂商二进制及其他不可公开的材料。

## 敏感报告

不要在公开 Issue 或 PR 中提交利用细节、凭据或私有系统信息。私下联系方法以当前
[Linkerbot 组织安全政策](https://github.com/linker-bot/.github/security/policy)为准。
本仓库不复制联系方式，避免组织政策变化后产生不同版本。

报告应包含仓库、完整提交、触发条件和前置条件、所需权限、实际影响和边界、
可安全分享的复现方式及临时缓解办法。不承诺固定响应时间。

涉及物理设备时，如出现意外运动或不安全输出，应停止测试。只测试自己拥有或获授权使用的系统和设备。
