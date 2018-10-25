// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "pch.h"
#include "../HCWebSocket.h"
#include <winrt/windows.foundation.h>
#include <winrt/windows.storage.streams.h>
#include <winrt/windows.networking.sockets.h>

using namespace xbox::httpclient;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;


class websocket_outgoing_message
{
public: 
    http_internal_string m_message;
    AsyncBlock* m_asyncBlock{ nullptr };
    DataWriterStoreOperation m_storeAsyncOp{ nullptr };
    AsyncStatus m_storeAsyncOpStatus;
    HRESULT m_storeAsyncResult;
    uint64_t m_id;
};

// This class is required by the implementation in order to function:
// The TypedEventHandler requires the message received and close handler to be a member of WinRT class.
class ReceiveContext
{
public:
    ReceiveContext() : m_websocket(nullptr)
    {
    }

    friend HRESULT WebsocketConnectDoWork(
        _Inout_ AsyncBlock* asyncBlock,
        _In_opt_ void* executionRoutineContext
        );

    void OnReceive(MessageWebSocket sender, MessageWebSocketMessageReceivedEventArgs args);
    void OnClosed(IWebSocket sender, WebSocketClosedEventArgs args);

private:
    hc_websocket_handle_t m_websocket;
};

class winrt_websocket_impl : public hc_websocket_impl
{
public:
    winrt_websocket_impl() : m_connectAsyncOpResult(S_OK)
    {
    }

    MessageWebSocket m_messageWebSocket;
    DataWriter m_messageDataWriter;
    HRESULT m_connectAsyncOpResult;
    ReceiveContext m_context;

    IAsyncAction m_connectAsyncOp;

    std::mutex m_outgoingMessageQueueLock;
    std::queue<std::shared_ptr<websocket_outgoing_message>> m_outgoingMessageQueue;
    hc_websocket_handle_t m_websocketHandle;
};

void MessageWebSocketSendMessage(
    _In_ std::shared_ptr<winrt_websocket_impl> websocketTask
    );

void ReceiveContext::OnReceive(MessageWebSocket sender, MessageWebSocketMessageReceivedEventArgs args)
{
    try
    {
        DataReader reader = args.GetDataReader();
        const auto len = reader.UnconsumedBufferLength();
        if (len > 0)
        {
            std::vector<uint8_t> payload;
            payload.resize(len);
            reader.ReadBytes(payload);
            HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: receieved msg [%s]", m_websocket->id, payload.data());

            HCWebSocketMessageFunction messageFunc = nullptr;
            HCWebSocketGetFunctions(&messageFunc, nullptr);
            if (messageFunc != nullptr)
            {
                messageFunc(m_websocket, reinterpret_cast<const char*>(payload.data()));
            }
        }
    }
    catch (...)
    {
    }
}

void ReceiveContext::OnClosed(IWebSocket sender, WebSocketClosedEventArgs args)
{
    HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: on closed event triggered", m_websocket->id);

    HCWebSocketCloseEventFunction closeFunc = nullptr;
    HCWebSocketGetFunctions(nullptr, &closeFunc);
    if (closeFunc != nullptr)
    {
        closeFunc(m_websocket, static_cast<HCWebSocketCloseStatus>(args.Code()));
    }
}

inline bool str_icmp(const char* left, const char* right)
{
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return boost::iequals(left, right);
#endif
}

http_internal_vector<http_internal_wstring> parse_subprotocols(const http_internal_string& subProtocol)
{
    http_internal_vector<http_internal_wstring> values;
    http_internal_wstring token;
    std::wstringstream header(utf16_from_utf8(subProtocol).c_str());

    while (std::getline(header, token, L','))
    {
        trim_whitespace(token);
        if (!token.empty())
        {
            values.push_back(token);
        }
    }

    return values;
}

HRESULT WebsocketConnectDoWork(
    _Inout_ AsyncBlock* asyncBlock,
    _In_opt_ void* executionRoutineContext
    )
try
{
    hc_websocket_handle_t websocket = static_cast<hc_websocket_handle_t>(executionRoutineContext);
    HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Connect executing", websocket->id);

    std::shared_ptr<winrt_websocket_impl> websocketTask = std::dynamic_pointer_cast<winrt_websocket_impl>(websocket->impl);
    websocketTask->m_messageWebSocket = MessageWebSocket();

    uint32_t numHeaders = 0;
    HCWebSocketGetNumHeaders(websocket, &numHeaders);

    http_internal_string protocolHeader("Sec-WebSocket-Protocol");
    for (uint32_t i = 0; i < numHeaders; i++)
    {
        const char* headerName;
        const char* headerValue;
        HCWebSocketGetHeaderAtIndex(websocket, i, &headerName, &headerValue);

        // The MessageWebSocket API throws a COMException if you try to set the
        // 'Sec-WebSocket-Protocol' header here. It requires you to go through their API instead.
        if (headerName != nullptr && headerValue != nullptr && !str_icmp(headerName, protocolHeader.c_str()))
        {
            websocketTask->m_messageWebSocket.SetRequestHeader(winrt::to_hstring(headerName), winrt::to_hstring(headerValue));
            HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Header %d [%s: %s]", websocket->id, i, headerName, headerValue);
        }
    }

    auto protocols = parse_subprotocols(websocket->subProtocol);
    for (const auto& value : protocols)
    {
        websocketTask->m_messageWebSocket.Control().SupportedProtocols().Append(value.c_str());
        HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Protocol [%S]", websocket->id, value.c_str());
    }

    websocketTask->m_context = ReceiveContext();
    websocketTask->m_context.m_websocket = websocket;

    http_internal_wstring aUrl = utf16_from_utf8(websocket->uri);
    const auto cxUri = winrt::Windows::Foundation::Uri(aUrl.c_str());

    websocketTask->m_messageWebSocket.MessageReceived([websocketTask](auto&& sender, auto&&args) { websocketTask->m_context.OnReceive(sender, args); });
    websocketTask->m_messageWebSocket.Closed([websocketTask](auto&& sender, auto&& args) { websocketTask->m_context.OnClosed(sender, args); });

    HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: connecting to %s", websocket->id, websocket->uri.c_str());

    try
    {
        websocketTask->m_connectAsyncOp = websocketTask->m_messageWebSocket.ConnectAsync(cxUri);

        websocketTask->m_connectAsyncOp.Completed(AsyncActionCompletedHandler(
            [websocket, websocketTask, asyncBlock](
                winrt::Windows::Foundation::IAsyncAction asyncOp,
                winrt::Windows::Foundation::AsyncStatus status)
        {
            UNREFERENCED_PARAMETER(status);
            try
            {
                websocketTask->m_messageDataWriter = DataWriter(websocketTask->m_messageWebSocket.OutputStream());
                if (status == winrt::Windows::Foundation::AsyncStatus::Error)
                {
                    websocketTask->m_connectAsyncOpResult = E_FAIL;
                }
                else
                {
                    websocketTask->m_connectAsyncOpResult = S_OK;
                }
            }
            catch (winrt::hresult_error e)
            {
                websocketTask->m_connectAsyncOpResult = e.code();
            }
            catch (...)
            {
                websocketTask->m_connectAsyncOpResult = E_FAIL;
            }
            if (FAILED(websocketTask->m_connectAsyncOpResult))
            {
                HC_TRACE_ERROR(WEBSOCKET, "Websocket [ID %llu]: connect failed 0x%0.8x", websocket->id, websocketTask->m_connectAsyncOpResult);
            }
            else
            {
                HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu] connect complete", websocket->id);
            }

            CompleteAsync(asyncBlock, S_OK, sizeof(WebSocketCompletionResult));
        }));
    }
    catch (winrt::hresult_error e)
    {
        HC_TRACE_ERROR(WEBSOCKET, "Websocket [ID %llu]: ConnectAsync failed = 0x%0.8x", websocketTask->m_websocketHandle->id, e.code());
        return e.code();
    }

    return E_PENDING;
}
CATCH_RETURN()

HRESULT WebsocketConnectGetResult(_In_ const AsyncProviderData* data)
{
    hc_websocket_handle_t websocket = static_cast<hc_websocket_handle_t>(data->context);
    std::shared_ptr<winrt_websocket_impl> websocketTask = std::dynamic_pointer_cast<winrt_websocket_impl>(websocket->impl);

    WebSocketCompletionResult result = {};
    result.websocket = websocket;
    result.errorCode = (FAILED(websocketTask->m_connectAsyncOpResult)) ? E_FAIL : S_OK;
    result.platformErrorCode = websocketTask->m_connectAsyncOpResult;
    CopyMemory(data->buffer, &result, sizeof(WebSocketCompletionResult));

    return S_OK;
}

HRESULT Internal_HCWebSocketConnectAsync(
    _In_z_ PCSTR uri,
    _In_z_ PCSTR subProtocol,
    _In_ hc_websocket_handle_t websocket,
    _Inout_ AsyncBlock* asyncBlock
    )
{
    std::shared_ptr<winrt_websocket_impl> websocketTask = std::make_shared<winrt_websocket_impl>();
    websocketTask->m_websocketHandle = HCWebSocketDuplicateHandle(websocket);
    websocket->uri = uri;
    websocket->subProtocol = subProtocol;
    websocket->impl = std::dynamic_pointer_cast<hc_websocket_impl>(websocketTask);

    HRESULT hr = BeginAsync(asyncBlock, websocket, HCWebSocketConnectAsync, __FUNCTION__,
        [](_In_ AsyncOp op, _In_ const AsyncProviderData* data)
    {
        switch (op)
        {
            case AsyncOp_DoWork: return WebsocketConnectDoWork(data->async, data->context);
            case AsyncOp_GetResult: return WebsocketConnectGetResult(data);
            case AsyncOp_Cleanup:
            {
                HCWebSocketCloseHandle(static_cast<hc_websocket_handle_t>(data->context));
                break;
            }
        }

        return S_OK;
    });

    if (hr == S_OK)
    {
        hr = ScheduleAsync(asyncBlock, 0);
    }

    return hr;
}

HRESULT Internal_HCWebSocketSendMessageAsync(
    _In_ hc_websocket_handle_t websocket,
    _In_z_ PCSTR message,
    _Inout_ AsyncBlock* asyncBlock
    )
{
    if (message == nullptr)
    {
        return E_INVALIDARG;
    }

    auto httpSingleton = get_http_singleton(false);
    if (nullptr == httpSingleton)
        return E_HC_NOT_INITIALISED;
    std::shared_ptr<winrt_websocket_impl> websocketTask = std::dynamic_pointer_cast<winrt_websocket_impl>(websocket->impl);

    std::shared_ptr<websocket_outgoing_message> msg = std::make_shared<websocket_outgoing_message>();
    msg->m_message = message;
    msg->m_asyncBlock = asyncBlock;
    msg->m_id = ++httpSingleton->m_lastId;

    if (msg->m_message.length() == 0)
    {
        return E_INVALIDARG;
    }

    bool sendInProgress = false;
    {
        std::lock_guard<std::mutex> lock(websocketTask->m_outgoingMessageQueueLock);
        if (websocketTask->m_outgoingMessageQueue.size() > 0)
        {
            sendInProgress = true;
        }
        HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: send msg queue size: %lld", websocketTask->m_websocketHandle->id, websocketTask->m_outgoingMessageQueue.size());

        websocketTask->m_outgoingMessageQueue.push(msg);
    }

    // No sends in progress, so start sending the message
    if (!sendInProgress)
    {
        MessageWebSocketSendMessage(websocketTask);
    }
    
    return S_OK;
}

struct SendMessageCallbackContent
{
    std::shared_ptr<websocket_outgoing_message> nextMessage;
    std::shared_ptr<winrt_websocket_impl> websocketTask;
};

HRESULT WebsockSendMessageDoWork(
    _Inout_ AsyncBlock* asyncBlock,
    _In_opt_ void* executionRoutineContext
    )
try
{
    auto sendMsgContext = shared_ptr_cache::fetch<SendMessageCallbackContent>(executionRoutineContext, false);
    if (sendMsgContext == nullptr)
    {
        HC_TRACE_ERROR(WEBSOCKET, "Websocket: Send message execute null");
        return E_INVALIDARG;
    }

    auto websocketTask = sendMsgContext->websocketTask;

    try
    {
        auto websocket = websocketTask->m_websocketHandle;
        HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Send message executing", websocket->id);

        auto msg = sendMsgContext->nextMessage;
        HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Message [ID %llu] [%s]", websocket->id, msg->m_id, msg->m_message.c_str());

        websocketTask->m_messageWebSocket.Control().MessageType(SocketMessageType::Utf8);
        unsigned char* uchar = reinterpret_cast<unsigned char*>(const_cast<char*>(msg->m_message.c_str()));
        websocketTask->m_messageDataWriter.WriteBytes(winrt::array_view(reinterpret_cast<const uint8_t*>(msg->m_message.data()), reinterpret_cast<const uint8_t*>(msg->m_message.data() + msg->m_message.size())));

        msg->m_storeAsyncOp = websocketTask->m_messageDataWriter.StoreAsync();

        msg->m_storeAsyncOp.Completed(AsyncOperationCompletedHandler<unsigned int>(
            [websocketTask, msg, asyncBlock](winrt::Windows::Foundation::IAsyncOperation<unsigned int> asyncOp, winrt::Windows::Foundation::AsyncStatus status)
        {
            try
            {
                msg->m_storeAsyncOpStatus = status;
                unsigned int result = asyncOp.GetResults();
                HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: Message [ID %llu] send complete = %d", websocketTask->m_websocketHandle->id, msg->m_id, result);
                msg->m_storeAsyncResult = result;
            }
            catch (winrt::hresult_error ex)
            {
                msg->m_storeAsyncResult = ex.code();
            }
            catch (...)
            {
                msg->m_storeAsyncResult = E_FAIL;
            }

            if (FAILED(msg->m_storeAsyncResult))
            {
                HC_TRACE_ERROR(WEBSOCKET, "Websocket [ID %llu]: Message [ID %llu] send failed = 0x%0.8x", websocketTask->m_websocketHandle->id, msg->m_id, msg->m_storeAsyncResult);
            }
            CompleteAsync(asyncBlock, msg->m_storeAsyncResult, sizeof(WebSocketCompletionResult));
            MessageWebSocketSendMessage(websocketTask);
        }));
    }
    catch (winrt::hresult_error e)
    {
        HC_TRACE_ERROR(WEBSOCKET, "Websocket [ID %llu]: Send failed = 0x%0.8x", websocketTask->m_websocketHandle->id, e.code());
        return e.code();
    }

    return E_PENDING;
}
CATCH_RETURN()

HRESULT WebsockSendMessageGetResult(_In_ const AsyncProviderData* data)
{
    if (data->context == nullptr ||
        data->bufferSize < sizeof(WebSocketCompletionResult))
    {
        return E_INVALIDARG;
    }

    auto sendMsgContext = shared_ptr_cache::fetch<SendMessageCallbackContent>(data->context, false);
    if (sendMsgContext == nullptr)
    {
        HC_TRACE_ERROR(WEBSOCKET, "Websocket GetResult null");
        return E_INVALIDARG;
    }

    auto msg = sendMsgContext->nextMessage;
    auto websocket = sendMsgContext->websocketTask->m_websocketHandle;

    if (websocket == nullptr)
    {
        return E_INVALIDARG;
    }

    HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: GetResult ", websocket->id);

    WebSocketCompletionResult result = {};
    result.websocket = websocket;
    result.errorCode = (FAILED(msg->m_storeAsyncResult)) ? E_FAIL : S_OK;
    result.platformErrorCode = msg->m_storeAsyncResult;
    CopyMemory(data->buffer, &result, sizeof(WebSocketCompletionResult));

    return S_OK;
}

void MessageWebSocketSendMessage(
    _In_ std::shared_ptr<winrt_websocket_impl> websocketTask
    )
{
    std::shared_ptr<websocket_outgoing_message> msg;

    {
        std::lock_guard<std::mutex> lock(websocketTask->m_outgoingMessageQueueLock);
        if (websocketTask->m_outgoingMessageQueue.size() > 0)
        {
            msg = websocketTask->m_outgoingMessageQueue.front();
            websocketTask->m_outgoingMessageQueue.pop();
        }
    }
    if (msg == nullptr)
    {
        return;
    }

    std::shared_ptr<SendMessageCallbackContent> callbackContext = std::make_shared<SendMessageCallbackContent>();
    callbackContext->nextMessage = msg;
    callbackContext->websocketTask = websocketTask;
    void* rawContext = shared_ptr_cache::store<SendMessageCallbackContent>(callbackContext);
    HCWebSocketDuplicateHandle(websocketTask->m_websocketHandle);

    HRESULT hr = BeginAsync(msg->m_asyncBlock, rawContext, HCWebSocketSendMessageAsync, __FUNCTION__,
        [](_In_ AsyncOp op, _In_ const AsyncProviderData* data)
    {
        switch (op)
        {
            case AsyncOp_DoWork: return WebsockSendMessageDoWork(data->async, data->context);
            case AsyncOp_GetResult: return WebsockSendMessageGetResult(data);
            case AsyncOp_Cleanup: 
            {
                HCWebSocketCloseHandle(shared_ptr_cache::fetch<SendMessageCallbackContent>(data->context, true)->websocketTask->m_websocketHandle);
                break;
            }
        }

        return S_OK;
    });

    if (hr == S_OK)
    {
        hr = ScheduleAsync(msg->m_asyncBlock, 0);
    }
}


HRESULT Internal_HCWebSocketDisconnect(
    _In_ hc_websocket_handle_t websocket,
    _In_ HCWebSocketCloseStatus closeStatus
    )
{
    if (websocket == nullptr)
    {
        return E_INVALIDARG;
    }

    std::shared_ptr<winrt_websocket_impl> websocketTask = std::dynamic_pointer_cast<winrt_websocket_impl>(websocket->impl);
    if (websocketTask == nullptr || websocketTask->m_messageWebSocket == nullptr)
    {
        return E_UNEXPECTED;
    }

    HC_TRACE_INFORMATION(WEBSOCKET, "Websocket [ID %llu]: disconnecting", websocket->id);
    websocketTask->m_messageWebSocket.Close(static_cast<unsigned short>(closeStatus), L"");
    return S_OK;
}

