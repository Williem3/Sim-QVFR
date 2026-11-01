#include "dred.h"

#include <atomic>
#include <iterator>
#include <string>
#include <thread>

#include <d3d12.h>

#include "../log.h"

namespace aceqv {
namespace {

std::atomic<bool> stopWatch{false};
std::thread watcher;
ID3D12Device* watched = nullptr;
bool reported = false;

const char* op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* const names[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
        "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
        "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
        "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission"};
    return static_cast<size_t>(op) < std::size(names) ? names[op] : nullptr;
}

void log_ops(const D3D12_AUTO_BREADCRUMB_OP* history, UINT count, UINT last) {
    const UINT from = last > 8 ? last - 8 : 0, to = last + 4 < count ? last + 4 : count;
    for (UINT i = from; i < to; ++i) {
        const char* name = op_name(history[i]);
        logf("dred:     op %u%s %s%s", i, i == last ? " <- last completed" : "", name ? name : "op",
             name ? "" : (" #" + std::to_string(history[i])).c_str());
    }
}

void log_allocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* node) {
    for (int n = 0; node && n < 12; node = node->pNext, ++n)
        logf("dred:   %s allocation type=%d name=%ls%s", what, node->AllocationType,
             node->ObjectNameW ? node->ObjectNameW : L"",
             node->ObjectNameA ? (std::string(" ") + node->ObjectNameA).c_str() : "");
}

}  // namespace

bool dred_enable() {
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))) || !settings) {
        log_line("dred: settings interface unavailable; no GPU fault report");
        return false;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings1))) && settings1) {
        settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings1->Release();
    }
    settings->Release();
    log_line("dred: auto-breadcrumbs, breadcrumb contexts and page-fault reporting on");
    return true;
}

bool dred_report_if_removed(ID3D12Device* device) {
    const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
    if (reason == S_OK || reported)
        return reason != S_OK;
    reported = true;
    logf("dred: DEVICE REMOVED, reason 0x%08lx", static_cast<unsigned long>(reason));
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        log_line("dred: no DRED data on this device (was DRED enabled before device creation?)");
        return true;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        int lists = 0, unfinished = 0;
        const D3D12_AUTO_BREADCRUMB_NODE1* recentDone[6] = {};  // completed lists just before the first unfinished
        int doneCount = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            ++lists;
            const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (last >= node->BreadcrumbCount) {
                if (!unfinished)
                    recentDone[doneCount++ % std::size(recentDone)] = node;
                continue;
            }
            if (!unfinished)
                for (int k = doneCount > 6 ? doneCount - 6 : 0; k < doneCount; ++k) {
                    const auto* done = recentDone[k % std::size(recentDone)];
                    logf("dred:   completed before it: '%ls%s' (%u ops)",
                         done->pCommandListDebugNameW ? done->pCommandListDebugNameW : L"",
                         done->pCommandListDebugNameA ? done->pCommandListDebugNameA : "", done->BreadcrumbCount);
                }
            if (unfinished >= 8)
                continue;
            ++unfinished;
            logf("dred:   unfinished command list '%ls%s' on queue '%ls%s': %u of %u ops completed",
                 node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"",
                 node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "",
                 node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"",
                 node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "", last, node->BreadcrumbCount);
            // Marker strings at or before the failing op: the passes that were running.
            int shown = 0;
            for (UINT c = node->BreadcrumbContextsCount; c-- > 0 && shown < 8;) {
                const D3D12_DRED_BREADCRUMB_CONTEXT& context = node->pBreadcrumbContexts[c];
                if (context.BreadcrumbIndex <= last + 3) {
                    logf("dred:     context at op %u: %ls", context.BreadcrumbIndex,
                         context.pContextString ? context.pContextString : L"");
                    ++shown;
                }
            }
            log_ops(node->pCommandHistory, node->BreadcrumbCount, last);
        }
        logf("dred:   %d command lists in the breadcrumb log, %d unfinished", lists, unfinished);
    } else {
        log_line("dred:   no breadcrumbs");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
        logf("dred:   page fault at GPU VA 0x%llx", static_cast<unsigned long long>(fault.PageFaultVA));
        log_allocations("existing", fault.pHeadExistingAllocationNode);
        log_allocations("recently freed", fault.pHeadRecentFreedAllocationNode);
    } else {
        log_line("dred:   no page fault recorded");
    }
    dred->Release();
    return true;
}

void dred_watch(ID3D12Device* device) {
    if (!device || watcher.joinable())
        return;
    device->AddRef();
    watched = device;
    stopWatch = false;
    watcher = std::thread([] {
        while (!stopWatch.load()) {
            if (dred_report_if_removed(watched))
                return;
            Sleep(50);
        }
    });
    log_line("dred: watching the device for removal");
}

namespace {

int perMessageId = 3;  // GPU-based validation reports many resources under one ID: more each
constexpr int kMaxMessages = 400;
SRWLOCK messagesLock = SRWLOCK_INIT;
int perId[2048];
int messagesLogged = 0;

void CALLBACK on_debug_message(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                               LPCSTR description, void*) {
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING)
        return;
    AcquireSRWLockExclusive(&messagesLock);
    const int slot = static_cast<int>(id) & 2047;
    const bool log = messagesLogged < kMaxMessages && perId[slot] < perMessageId;
    if (log) {
        ++perId[slot];
        ++messagesLogged;
    }
    ReleaseSRWLockExclusive(&messagesLock);
    if (!log)
        return;
    static const char* const severities[] = {"CORRUPTION", "ERROR", "WARNING"};
    logf("d3d12 debug %s id=%d category=%d: %s", severities[severity], static_cast<int>(id), static_cast<int>(category),
         description ? description : "");
}

}  // namespace

bool d3d12_debug_enable(bool gpuValidation) {
    ID3D12Debug* debug = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) {
        log_line("d3d12 debug: debug layer unavailable (Graphics Tools not installed?)");
        return false;
    }
    debug->EnableDebugLayer();
    ID3D12Debug1* debug1 = nullptr;
    if (gpuValidation && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))) && debug1) {
        debug1->SetEnableGPUBasedValidation(TRUE);
        debug1->Release();
        perMessageId = 12;
    }
    debug->Release();
    log_line(gpuValidation ? "d3d12 debug: debug layer and GPU-based validation on (very slow; for diagnosis only)"
                           : "d3d12 debug: debug layer on (slow; for diagnosis only)");
    return true;
}

void d3d12_debug_attach(ID3D12Device* device) {
    ID3D12InfoQueue1* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
        log_line("d3d12 debug: no ID3D12InfoQueue1 on the device (debug layer not active?)");
        return;
    }
    DWORD cookie = 0;
    const HRESULT hr = queue->RegisterMessageCallback(&on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    queue->Release();
    logf("d3d12 debug: message callback %s", SUCCEEDED(hr) ? "registered" : "failed");
}

void dred_stop() {
    if (!watcher.joinable())
        return;
    stopWatch = true;
    watcher.join();
    watched->Release();
    watched = nullptr;
}

}  // namespace aceqv
