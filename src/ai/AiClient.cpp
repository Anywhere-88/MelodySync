// ============================================================================
//  src/ai/AiClient.cpp
// ============================================================================
#include "ai/AiClient.h"

#include "core/HttpClient.h"
#include "settings/Settings.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace ms {

AiClient::AiClient() { reloadFromSettings(); }

void AiClient::configure(Provider p, const QString& baseUrl, const QString& apiKey,
                         const QString& model, int timeoutMs)
{
    m_provider  = p;
    m_baseUrl   = baseUrl.trimmed();
    m_apiKey    = apiKey.trimmed();
    m_model     = model.trimmed();
    m_timeoutMs = timeoutMs;
    while (m_baseUrl.endsWith(QLatin1Char('/'))) m_baseUrl.chop(1);
}

void AiClient::reloadFromSettings()
{
    const Settings& s = Settings::instance();
    configure(providerFromName(s.aiProvider()), s.aiBaseUrl(), s.aiApiKey(),
              s.aiModel(), s.aiTimeoutMs());
}

AiClient::Provider AiClient::providerFromName(const QString& name)
{
    const QString n = name.toLower();
    if (n == QLatin1String("claude") || n == QLatin1String("anthropic"))
        return Provider::Claude;
    if (n == QLatin1String("glm") || n == QLatin1String("zhipu") || n == QLatin1String("bigmodel"))
        return Provider::Glm;
    if (n == QLatin1String("custom"))
        return Provider::Custom;
    return Provider::OpenAI;
}

QString AiClient::providerName() const
{
    switch (m_provider) {
    case Provider::Claude: return QStringLiteral("Claude");
    case Provider::Glm:    return QStringLiteral("GLM");
    case Provider::Custom: return QStringLiteral("Custom");
    default:               return QStringLiteral("OpenAI");
    }
}

// ---------------------------------------------------------------------------
AiClient::Reply AiClient::callOpenAiCompatible(const QString& systemPrompt,
                                               const QString& userPrompt,
                                               double temperature, int maxTokens)
{
    Reply reply;
    if (m_baseUrl.isEmpty()) { reply.error = QStringLiteral("未配置 API 地址"); return reply; }

    QJsonArray messages;
    {
        QJsonObject sys;
        sys[QStringLiteral("role")]    = QStringLiteral("system");
        sys[QStringLiteral("content")] = systemPrompt;
        messages.append(sys);

        QJsonObject usr;
        usr[QStringLiteral("role")]    = QStringLiteral("user");
        usr[QStringLiteral("content")] = userPrompt;
        messages.append(usr);
    }

    QJsonObject body;
    body[QStringLiteral("model")]       = m_model;
    body[QStringLiteral("messages")]    = messages;
    body[QStringLiteral("temperature")] = temperature;
    body[QStringLiteral("max_tokens")]  = maxTokens;
    body[QStringLiteral("stream")]      = false;

    QStringList headers;
    headers << (QStringLiteral("Authorization: Bearer ") + m_apiKey);

    const QString url = m_baseUrl + QStringLiteral("/chat/completions");
    const HttpClient::Response r = HttpClient::postJson(url, body, headers, m_timeoutMs);
    if (!r.ok) {
        // 尽量把**服务端给的具体原因**带出来。
        // 智谱对无效 Key 返回的是「令牌已过期或验证不正确」这类明确信息，
        // 比笼统的「API Key 无效」有用得多 —— 用户能自己判断是过期、复制漏了
        // 后半段、还是额度问题。原来直接覆盖成固定文案，等于把最有用的线索丢了。
        const QString detail = r.json().value(QStringLiteral("error")).toObject()
                                   .value(QStringLiteral("message")).toString();
        if (r.status == 401 || r.status == 403) {
            reply.error = detail.isEmpty()
                ? QStringLiteral("API Key 无效或已过期（HTTP %1）").arg(r.status)
                : QStringLiteral("API Key 无效或已过期：%1").arg(detail);
        } else if (r.status == 429) {
            reply.error = detail.isEmpty() ? QStringLiteral("请求过于频繁，请稍后再试")
                                           : QStringLiteral("请求被限流：%1").arg(detail);
        } else {
            reply.error = r.error.isEmpty() ? QStringLiteral("请求失败") : r.error;
            if (!detail.isEmpty()) reply.error += QStringLiteral("（%1）").arg(detail);
        }
        return reply;
    }

    const QJsonObject o = r.json();
    const QJsonArray choices = o.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) { reply.error = QStringLiteral("响应为空"); return reply; }

    reply.text = choices.at(0).toObject()
                     .value(QStringLiteral("message")).toObject()
                     .value(QStringLiteral("content")).toString();

    const QJsonObject usage = o.value(QStringLiteral("usage")).toObject();
    reply.promptTokens     = usage.value(QStringLiteral("prompt_tokens")).toInt();
    reply.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();

    reply.ok = !reply.text.isEmpty();
    if (!reply.ok && reply.error.isEmpty()) reply.error = QStringLiteral("模型未返回内容");
    return reply;
}

// ---------------------------------------------------------------------------
AiClient::Reply AiClient::callClaude(const QString& systemPrompt,
                                     const QString& userPrompt,
                                     double temperature, int maxTokens)
{
    Reply reply;
    if (m_baseUrl.isEmpty()) { reply.error = QStringLiteral("未配置 API 地址"); return reply; }

    QJsonArray messages;
    {
        QJsonObject usr;
        usr[QStringLiteral("role")]    = QStringLiteral("user");
        usr[QStringLiteral("content")] = userPrompt;
        messages.append(usr);
    }

    QJsonObject body;
    body[QStringLiteral("model")]       = m_model;
    body[QStringLiteral("system")]      = systemPrompt;
    body[QStringLiteral("messages")]    = messages;
    body[QStringLiteral("temperature")] = temperature;
    body[QStringLiteral("max_tokens")]  = maxTokens;

    QStringList headers;
    headers << (QStringLiteral("x-api-key: ") + m_apiKey);
    headers << QStringLiteral("anthropic-version: 2023-06-01");

    const QString url = m_baseUrl + QStringLiteral("/messages");
    const HttpClient::Response r = HttpClient::postJson(url, body, headers, m_timeoutMs);
    if (!r.ok) {
        reply.error = r.error.isEmpty() ? QStringLiteral("请求失败") : r.error;
        return reply;
    }

    const QJsonObject o = r.json();
    const QJsonArray content = o.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& v : content) {
        const QJsonObject blk = v.toObject();
        if (blk.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            reply.text += blk.value(QStringLiteral("text")).toString();
    }

    const QJsonObject usage = o.value(QStringLiteral("usage")).toObject();
    reply.promptTokens     = usage.value(QStringLiteral("input_tokens")).toInt();
    reply.completionTokens = usage.value(QStringLiteral("output_tokens")).toInt();

    reply.ok = !reply.text.isEmpty();
    if (!reply.ok && reply.error.isEmpty()) reply.error = QStringLiteral("模型未返回内容");
    return reply;
}

AiClient::Reply AiClient::chat(const QString& systemPrompt, const QString& userPrompt,
                               double temperature, int maxTokens)
{
    reloadFromSettings();     // 用户可能刚在设置页填了 Key
    if (m_apiKey.isEmpty()) {
        Reply r;
        r.error = QStringLiteral("未配置 AI API Key（设置 → AI 搜索）");
        return r;
    }
    if (m_provider == Provider::Claude)
        return callClaude(systemPrompt, userPrompt, temperature, maxTokens);
    return callOpenAiCompatible(systemPrompt, userPrompt, temperature, maxTokens);
}

// ---------------------------------------------------------------------------
QString AiClient::extractJsonObject(const QString& raw)
{
    QString s = raw.trimmed();

    // 1) 去掉 ```json ... ``` / ``` ... ``` 围栏
    static const QRegularExpression fence(
        QStringLiteral("```(?:json|JSON)?\\s*([\\s\\S]*?)```"));
    const QRegularExpressionMatch m = fence.match(s);
    if (m.hasMatch()) s = m.captured(1).trimmed();

    // 2) 从第一个 '{' 到最后一个 '}' —— 模型常会在 JSON 前后写解释性文字
    const int b = s.indexOf(QLatin1Char('{'));
    const int e = s.lastIndexOf(QLatin1Char('}'));
    if (b >= 0 && e > b) s = s.mid(b, e - b + 1);

    return s.trimmed();
}

QJsonObject AiClient::chatJson(const QString& systemPrompt, const QString& userPrompt,
                               QString* errorOut, double temperature)
{
    const Reply r = chat(systemPrompt, userPrompt, temperature);
    if (!r.ok) {
        if (errorOut) *errorOut = r.error;
        return QJsonObject();
    }

    const QString json = extractJsonObject(r.text);
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) *errorOut = QStringLiteral("模型返回的 JSON 无法解析：") + perr.errorString();
        qWarning() << "[AiClient] JSON 解析失败，原文:" << r.text.left(300);
        return QJsonObject();
    }
    return doc.object();
}

} // namespace ms
