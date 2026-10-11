// ============================================================================
//  src/ai/AiClient.h
//  统一的 LLM 调用客户端（OpenAI / Claude / GLM 兼容协议）。
//
//  只做一件事：把 (system, user) 两个提示词送出去，把纯文本拿回来。
//  提示词工程与结果解析交给 ai/IntentParser。
// ============================================================================
#pragma once

#include <QJsonObject>
#include <QString>

namespace ms {

class AiClient
{
public:
    enum class Provider { OpenAI, Claude, Glm, Custom };

    struct Reply
    {
        bool    ok = false;
        QString text;
        QString error;
        int     promptTokens = 0;
        int     completionTokens = 0;
    };

    AiClient();

    void configure(Provider p, const QString& baseUrl, const QString& apiKey,
                   const QString& model, int timeoutMs = 20000);

    // 从 Settings 读取配置（每次调用前刷新，用户改设置立即生效）
    void reloadFromSettings();

    bool    ready() const { return !m_apiKey.isEmpty(); }
    QString providerName() const;
    QString model() const { return m_model; }

    // 普通对话补全
    Reply chat(const QString& systemPrompt,
               const QString& userPrompt,
               double temperature = 0.2,
               int maxTokens = 1024);

    // 要求模型输出 JSON：自动剥离 ```json 代码块围栏，并做一次宽松修复
    QJsonObject chatJson(const QString& systemPrompt,
                         const QString& userPrompt,
                         QString* errorOut = nullptr,
                         double temperature = 0.1);

    static Provider providerFromName(const QString& name);

private:
    Reply callOpenAiCompatible(const QString& systemPrompt, const QString& userPrompt,
                               double temperature, int maxTokens);
    Reply callClaude(const QString& systemPrompt, const QString& userPrompt,
                     double temperature, int maxTokens);

    // 从可能带 markdown 围栏 / 前后废话的文本里抠出第一个完整 JSON 对象
    static QString extractJsonObject(const QString& raw);

    Provider m_provider = Provider::OpenAI;
    QString  m_baseUrl;
    QString  m_apiKey;
    QString  m_model;
    int      m_timeoutMs = 20000;
};

} // namespace ms
