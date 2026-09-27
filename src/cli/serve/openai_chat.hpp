#ifndef GUFO_SERVER_OPENAI_CHAT_HPP_
#define GUFO_SERVER_OPENAI_CHAT_HPP_

#include <string>
#include <string_view>

#include "src/cli/serve/http_server.hpp"
#include "src/core/json.hpp"

namespace gufo::server {

/// Handles the supported OpenAI Chat Completions subset. Streaming responses
/// consume scheduler-published token pieces from HttpResponse::streaming_body;
/// socket writes never own or execute model state.
HttpResponse HandleOpenAiChat(const HttpRequest& request,
                              TextGenerationBackend& backend);

/// Responses text output uses the same reasoning/UTF-8 filter and scheduler
/// as Chat Completions, including streaming cancellation and cache retention.
HttpResponse CreateOpenAiResponse(const HttpRequest& request,
                                  TextGenerationBackend& backend,
                                  const ChatRequest& chat,
                                  std::size_t max_tokens,
                                  const sampling::SamplingConfig& sampling,
                                  bool stream);

/// Reads the reasoning controls a chat request may carry (Anthropic
/// `thinking`, `reasoning_effort`, `chat_template_kwargs`) and fills the ones
/// it leaves unset from the backend defaults. Returns false with *error set
/// for an invalid request.
bool ReadChatReasoningOptions(const json::Value& body,
                              const TextGenerationBackend& backend,
                              ReasoningOptions* options, std::string* error);

struct ReasoningSplit {
  std::string reasoning;
  std::string text;
};

/// Separates a completed chat generation into its reasoning and its visible
/// answer, for protocols that return the two in separate fields.
ReasoningSplit SplitReasoning(
    std::string_view raw,
    TextGenerationBackend::InitialOutputState initial_output_state);

}  // namespace gufo::server

#endif  // GUFO_SERVER_OPENAI_CHAT_HPP_
