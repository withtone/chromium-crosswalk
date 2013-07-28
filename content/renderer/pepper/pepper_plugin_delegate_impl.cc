// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/pepper/pepper_plugin_delegate_impl.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <queue>

#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util_proxy.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/sync_socket.h"
#include "base/time/time.h"
#include "content/child/child_process.h"
#include "content/child/fileapi/file_system_dispatcher.h"
#include "content/child/npapi/webplugin.h"
#include "content/common/child_process_messages.h"
#include "content/common/fileapi/file_system_messages.h"
#include "content/common/gpu/client/context_provider_command_buffer.h"
#include "content/common/gpu/client/webgraphicscontext3d_command_buffer_impl.h"
#include "content/common/pepper_messages.h"
#include "content/common/sandbox_util.h"
#include "content/common/view_messages.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/context_menu_params.h"
#include "content/public/common/media_stream_request.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/referrer.h"
#include "content/public/common/webplugininfo.h"
#include "content/public/renderer/content_renderer_client.h"
#include "content/renderer/gamepad_shared_memory_reader.h"
#include "content/renderer/media/media_stream_dispatcher.h"
#include "content/renderer/media/pepper_platform_video_decoder_impl.h"
#include "content/renderer/p2p/socket_dispatcher.h"
#include "content/renderer/pepper/content_renderer_pepper_host_factory.h"
#include "content/renderer/pepper/host_dispatcher_wrapper.h"
#include "content/renderer/pepper/pepper_broker_impl.h"
#include "content/renderer/pepper/pepper_browser_connection.h"
#include "content/renderer/pepper/pepper_file_system_host.h"
#include "content/renderer/pepper/pepper_graphics_2d_host.h"
#include "content/renderer/pepper/pepper_hung_plugin_filter.h"
#include "content/renderer/pepper/pepper_in_process_resource_creation.h"
#include "content/renderer/pepper/pepper_in_process_router.h"
#include "content/renderer/pepper/pepper_platform_audio_output_impl.h"
#include "content/renderer/pepper/pepper_plugin_instance_impl.h"
#include "content/renderer/pepper/pepper_plugin_registry.h"
#include "content/renderer/pepper/pepper_url_loader_host.h"
#include "content/renderer/pepper/pepper_webplugin_impl.h"
#include "content/renderer/pepper/plugin_module.h"
#include "content/renderer/pepper/ppb_tcp_server_socket_private_impl.h"
#include "content/renderer/pepper/ppb_tcp_socket_private_impl.h"
#include "content/renderer/pepper/renderer_ppapi_host_impl.h"
#include "content/renderer/pepper/resource_helper.h"
#include "content/renderer/pepper/url_response_info_util.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/render_view_impl.h"
#include "content/renderer/render_widget_fullscreen_pepper.h"
#include "content/renderer/webplugin_delegate_proxy.h"
#include "ipc/ipc_channel_handle.h"
#include "media/base/audio_hardware_config.h"
#include "media/video/capture/video_capture_proxy.h"
#include "ppapi/c/dev/pp_video_dev.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/c/private/ppb_flash.h"
#include "ppapi/host/ppapi_host.h"
#include "ppapi/proxy/ppapi_messages.h"
#include "ppapi/proxy/url_loader_resource.h"
#include "ppapi/shared_impl/api_id.h"
#include "ppapi/shared_impl/file_path.h"
#include "ppapi/shared_impl/platform_file.h"
#include "ppapi/shared_impl/ppapi_globals.h"
#include "ppapi/shared_impl/ppapi_preferences.h"
#include "ppapi/shared_impl/ppb_device_ref_shared.h"
#include "ppapi/shared_impl/ppp_instance_combined.h"
#include "ppapi/shared_impl/resource_tracker.h"
#include "ppapi/shared_impl/socket_option_data.h"
#include "ppapi/thunk/enter.h"
#include "ppapi/thunk/ppb_tcp_server_socket_private_api.h"
#include "third_party/WebKit/public/web/WebCursorInfo.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebInputEvent.h"
#include "third_party/WebKit/public/web/WebScreenInfo.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "ui/gfx/size.h"
#include "url/gurl.h"

using WebKit::WebFrame;

namespace content {

namespace {

class PluginInstanceLockTarget : public MouseLockDispatcher::LockTarget {
 public:
  PluginInstanceLockTarget(PepperPluginInstanceImpl* plugin)
      : plugin_(plugin) {}

  virtual void OnLockMouseACK(bool succeeded) OVERRIDE {
    plugin_->OnLockMouseACK(succeeded);
  }

  virtual void OnMouseLockLost() OVERRIDE {
    plugin_->OnMouseLockLost();
  }

  virtual bool HandleMouseLockedInputEvent(
      const WebKit::WebMouseEvent &event) OVERRIDE {
    plugin_->HandleMouseLockedInputEvent(event);
    return true;
  }

 private:
  PepperPluginInstanceImpl* plugin_;
};

void DoNotifyCloseFile(int file_open_id, base::PlatformFileError /* unused */) {
  ChildThread::current()->file_system_dispatcher()->NotifyCloseFile(
      file_open_id);
}

void DidOpenFileSystemURL(
    const PluginDelegate::AsyncOpenFileSystemURLCallback& callback,
    base::PlatformFile file,
    int file_open_id,
    quota::QuotaLimitType quota_policy) {
  callback.Run(base::PLATFORM_FILE_OK,
               base::PassPlatformFile(&file),
               quota_policy,
               base::Bind(&DoNotifyCloseFile, file_open_id));
  // Make sure we won't leak file handle if the requester has died.
  if (file != base::kInvalidPlatformFileValue) {
    base::FileUtilProxy::Close(
        RenderThreadImpl::current()->GetFileThreadMessageLoopProxy().get(),
        file,
        base::Bind(&DoNotifyCloseFile, file_open_id));
  }
}

void DidFailOpenFileSystemURL(
    const PluginDelegate::AsyncOpenFileSystemURLCallback& callback,
    base::PlatformFileError error_code) {
  base::PlatformFile invalid_file = base::kInvalidPlatformFileValue;
  callback.Run(error_code,
               base::PassPlatformFile(&invalid_file),
               quota::kQuotaLimitTypeUnknown,
               PluginDelegate::NotifyCloseFileCallback());
}

void CreateHostForInProcessModule(RenderViewImpl* render_view,
                                  PluginModule* module,
                                  const WebPluginInfo& webplugin_info) {
  // First time an in-process plugin was used, make a host for it.
  const PepperPluginInfo* info =
      PepperPluginRegistry::GetInstance()->GetInfoForPlugin(webplugin_info);
  DCHECK(!info->is_out_of_process);

  ppapi::PpapiPermissions perms(
      PepperPluginRegistry::GetInstance()->GetInfoForPlugin(
          webplugin_info)->permissions);
  RendererPpapiHostImpl* host_impl =
      RendererPpapiHostImpl::CreateOnModuleForInProcess(
          module, perms);
  render_view->PpapiPluginCreated(host_impl);
}

}  // namespace

ppapi::host::ResourceHost* PepperPluginDelegateImpl::GetRendererResourceHost(
    PP_Instance instance, PP_Resource resource) {
  const ppapi::host::PpapiHost* ppapi_host =
      RendererPpapiHost::GetForPPInstance(instance)->GetPpapiHost();
  if (!resource || !ppapi_host)
    return NULL;
  return ppapi_host->GetResourceHost(resource);
}

PepperPluginDelegateImpl::PepperPluginDelegateImpl(RenderViewImpl* render_view)
    : RenderViewObserver(render_view),
      render_view_(render_view),
      pepper_browser_connection_(this),
      focused_plugin_(NULL),
      last_mouse_event_target_(NULL) {
}

PepperPluginDelegateImpl::~PepperPluginDelegateImpl() {
  DCHECK(mouse_lock_instances_.empty());
}

WebKit::WebPlugin* PepperPluginDelegateImpl::CreatePepperWebPlugin(
    const WebPluginInfo& webplugin_info,
    const WebKit::WebPluginParams& params) {
  bool pepper_plugin_was_registered = false;
  scoped_refptr<PluginModule> pepper_module(
      CreatePepperPluginModule(webplugin_info, &pepper_plugin_was_registered));

  if (pepper_plugin_was_registered) {
    if (!pepper_module.get())
      return NULL;
    return new PepperWebPluginImpl(
        pepper_module.get(), params, AsWeakPtr(), render_view_->AsWeakPtr());
  }

  return NULL;
}

scoped_refptr<PluginModule> PepperPluginDelegateImpl::CreatePepperPluginModule(
    const WebPluginInfo& webplugin_info,
    bool* pepper_plugin_was_registered) {
  *pepper_plugin_was_registered = true;

  // See if a module has already been loaded for this plugin.
  base::FilePath path(webplugin_info.path);
  scoped_refptr<PluginModule> module =
      PepperPluginRegistry::GetInstance()->GetLiveModule(path);
  if (module.get()) {
    if (!module->GetEmbedderState()) {
      // If the module exists and no embedder state was associated with it,
      // then the module was one of the ones preloaded and is an in-process
      // plugin. We need to associate our host state with it.
      CreateHostForInProcessModule(render_view_, module.get(), webplugin_info);
    }
    return module;
  }

  // In-process plugins will have always been created up-front to avoid the
  // sandbox restrictions. So getting here implies it doesn't exist or should
  // be out of process.
  const PepperPluginInfo* info =
      PepperPluginRegistry::GetInstance()->GetInfoForPlugin(webplugin_info);
  if (!info) {
    *pepper_plugin_was_registered = false;
    return scoped_refptr<PluginModule>();
  } else if (!info->is_out_of_process) {
    // In-process plugin not preloaded, it probably couldn't be initialized.
    return scoped_refptr<PluginModule>();
  }

  ppapi::PpapiPermissions permissions =
      ppapi::PpapiPermissions::GetForCommandLine(info->permissions);

  // Out of process: have the browser start the plugin process for us.
  IPC::ChannelHandle channel_handle;
  base::ProcessId peer_pid;
  int plugin_child_id = 0;
  render_view_->Send(new ViewHostMsg_OpenChannelToPepperPlugin(
      path, &channel_handle, &peer_pid, &plugin_child_id));
  if (channel_handle.name.empty()) {
    // Couldn't be initialized.
    return scoped_refptr<PluginModule>();
  }

  // AddLiveModule must be called before any early returns since the
  // module's destructor will remove itself.
  module = new PluginModule(info->name, path, permissions);
  PepperPluginRegistry::GetInstance()->AddLiveModule(path, module.get());

  if (!CreateOutOfProcessModule(module.get(),
                                path,
                                permissions,
                                channel_handle,
                                peer_pid,
                                plugin_child_id,
                                false))  // is_external = false
    return scoped_refptr<PluginModule>();

  return module;
}

scoped_refptr<PepperBrokerImpl> PepperPluginDelegateImpl::CreateBroker(
    PluginModule* plugin_module) {
  DCHECK(plugin_module);
  DCHECK(!plugin_module->GetBroker());

  // The broker path is the same as the plugin.
  const base::FilePath& broker_path = plugin_module->path();

  scoped_refptr<PepperBrokerImpl> broker =
      new PepperBrokerImpl(plugin_module, this);

  int request_id =
      pending_connect_broker_.Add(new scoped_refptr<PepperBrokerImpl>(broker));

  // Have the browser start the broker process for us.
  IPC::Message* msg =
      new ViewHostMsg_OpenChannelToPpapiBroker(render_view_->routing_id(),
                                               request_id,
                                               broker_path);
  if (!render_view_->Send(msg)) {
    pending_connect_broker_.Remove(request_id);
    return scoped_refptr<PepperBrokerImpl>();
  }

  return broker;
}

RendererPpapiHost* PepperPluginDelegateImpl::CreateOutOfProcessModule(
    PluginModule* module,
    const base::FilePath& path,
    ppapi::PpapiPermissions permissions,
    const IPC::ChannelHandle& channel_handle,
    base::ProcessId peer_pid,
    int plugin_child_id,
    bool is_external) {
  scoped_refptr<PepperHungPluginFilter> hung_filter(
      new PepperHungPluginFilter(path,
                                 render_view_->routing_id(),
                                 plugin_child_id));
  scoped_ptr<HostDispatcherWrapper> dispatcher(
      new HostDispatcherWrapper(module,
                                peer_pid,
                                plugin_child_id,
                                permissions,
                                is_external));
  if (!dispatcher->Init(
          channel_handle,
          PluginModule::GetLocalGetInterfaceFunc(),
          GetPreferences(),
          hung_filter.get()))
    return NULL;

  RendererPpapiHostImpl* host_impl =
      RendererPpapiHostImpl::CreateOnModuleForOutOfProcess(
          module, dispatcher->dispatcher(), permissions);
  render_view_->PpapiPluginCreated(host_impl);

  module->InitAsProxied(dispatcher.release());
  return host_impl;
}

void PepperPluginDelegateImpl::OnPpapiBrokerChannelCreated(
    int request_id,
    base::ProcessId broker_pid,
    const IPC::ChannelHandle& handle) {
  scoped_refptr<PepperBrokerImpl>* broker_ptr =
      pending_connect_broker_.Lookup(request_id);
  if (broker_ptr) {
    scoped_refptr<PepperBrokerImpl> broker = *broker_ptr;
    pending_connect_broker_.Remove(request_id);
    broker->OnBrokerChannelConnected(broker_pid, handle);
  } else {
    // There is no broker waiting for this channel. Close it so the broker can
    // clean up and possibly exit.
    // The easiest way to clean it up is to just put it in an object
    // and then close them. This failure case is not performance critical.
    PepperBrokerDispatcherWrapper temp_dispatcher;
    temp_dispatcher.Init(broker_pid, handle);
  }
}

// Iterates through pending_connect_broker_ to find the broker.
// Cannot use Lookup() directly because pending_connect_broker_ does not store
// the raw pointer to the broker. Assumes maximum of one copy of broker exists.
bool PepperPluginDelegateImpl::StopWaitingForBrokerConnection(
    PepperBrokerImpl* broker) {
  for (BrokerMap::iterator i(&pending_connect_broker_);
       !i.IsAtEnd(); i.Advance()) {
    if (i.GetCurrentValue()->get() == broker) {
      pending_connect_broker_.Remove(i.GetCurrentKey());
      return true;
    }
  }

  return false;
}

void PepperPluginDelegateImpl::ViewWillInitiatePaint() {
  // Notify all of our instances that we started painting. This is used for
  // internal bookkeeping only, so we know that the set can not change under
  // us.
  for (std::set<PepperPluginInstanceImpl*>::iterator i =
           active_instances_.begin();
       i != active_instances_.end(); ++i)
    (*i)->ViewWillInitiatePaint();
}

void PepperPluginDelegateImpl::ViewInitiatedPaint() {
  // Notify all instances that we painted.  The same caveats apply as for
  // ViewFlushedPaint regarding instances closing themselves, so we take
  // similar precautions.
  std::set<PepperPluginInstanceImpl*> plugins = active_instances_;
  for (std::set<PepperPluginInstanceImpl*>::iterator i = plugins.begin();
       i != plugins.end(); ++i) {
    if (active_instances_.find(*i) != active_instances_.end())
      (*i)->ViewInitiatedPaint();
  }
}

void PepperPluginDelegateImpl::ViewFlushedPaint() {
  // Notify all instances that we flushed. This will call into the plugin, and
  // we it may ask to close itself as a result. This will, in turn, modify our
  // set, possibly invalidating the iterator. So we iterate on a copy that
  // won't change out from under us.
  std::set<PepperPluginInstanceImpl*> plugins = active_instances_;
  for (std::set<PepperPluginInstanceImpl*>::iterator i = plugins.begin();
       i != plugins.end(); ++i) {
    // The copy above makes sure our iterator is never invalid if some plugins
    // are destroyed. But some plugin may decide to close all of its views in
    // response to a paint in one of them, so we need to make sure each one is
    // still "current" before using it.
    //
    // It's possible that a plugin was destroyed, but another one was created
    // with the same address. In this case, we'll call ViewFlushedPaint on that
    // new plugin. But that's OK for this particular case since we're just
    // notifying all of our instances that the view flushed, and the new one is
    // one of our instances.
    //
    // What about the case where a new one is created in a callback at a new
    // address and we don't issue the callback? We're still OK since this
    // callback is used for flush callbacks and we could not have possibly
    // started a new paint (ViewWillInitiatePaint) for the new plugin while
    // processing a previous paint for an existing one.
    if (active_instances_.find(*i) != active_instances_.end())
      (*i)->ViewFlushedPaint();
  }
}

PepperPluginInstanceImpl* PepperPluginDelegateImpl::
    GetBitmapForOptimizedPluginPaint(
        const gfx::Rect& paint_bounds,
        TransportDIB** dib,
        gfx::Rect* location,
        gfx::Rect* clip,
        float* scale_factor) {
  for (std::set<PepperPluginInstanceImpl*>::iterator i =
           active_instances_.begin();
       i != active_instances_.end(); ++i) {
    PepperPluginInstanceImpl* instance = *i;
    // In Flash fullscreen , the plugin contents should be painted onto the
    // fullscreen widget instead of the web page.
    if (!instance->FlashIsFullscreenOrPending() &&
        instance->GetBitmapForOptimizedPluginPaint(paint_bounds, dib, location,
                                                   clip, scale_factor))
      return *i;
  }
  return NULL;
}

void PepperPluginDelegateImpl::PluginFocusChanged(
    PepperPluginInstanceImpl* instance,
    bool focused) {
  if (focused)
    focused_plugin_ = instance;
  else if (focused_plugin_ == instance)
    focused_plugin_ = NULL;
  if (render_view_)
    render_view_->PpapiPluginFocusChanged();
}

void PepperPluginDelegateImpl::PluginTextInputTypeChanged(
    PepperPluginInstanceImpl* instance) {
  if (focused_plugin_ == instance && render_view_)
    render_view_->PpapiPluginTextInputTypeChanged();
}

void PepperPluginDelegateImpl::PluginCaretPositionChanged(
    PepperPluginInstanceImpl* instance) {
  if (focused_plugin_ == instance && render_view_)
    render_view_->PpapiPluginCaretPositionChanged();
}

void PepperPluginDelegateImpl::PluginRequestedCancelComposition(
    PepperPluginInstanceImpl* instance) {
  if (focused_plugin_ == instance && render_view_)
    render_view_->PpapiPluginCancelComposition();
}

void PepperPluginDelegateImpl::PluginSelectionChanged(
    PepperPluginInstanceImpl* instance) {
  if (focused_plugin_ == instance && render_view_)
    render_view_->PpapiPluginSelectionChanged();
}

void PepperPluginDelegateImpl::SimulateImeSetComposition(
    const string16& text,
    const std::vector<WebKit::WebCompositionUnderline>& underlines,
    int selection_start,
    int selection_end) {
  if (render_view_) {
    render_view_->SimulateImeSetComposition(
        text, underlines, selection_start, selection_end);
  }
}

void PepperPluginDelegateImpl::SimulateImeConfirmComposition(
    const string16& text) {
  if (render_view_)
    render_view_->SimulateImeConfirmComposition(text, ui::Range());
}

void PepperPluginDelegateImpl::OnImeSetComposition(
    const string16& text,
    const std::vector<WebKit::WebCompositionUnderline>& underlines,
    int selection_start,
    int selection_end) {
  if (!IsPluginAcceptingCompositionEvents()) {
    composition_text_ = text;
  } else {
    // TODO(kinaba) currently all composition events are sent directly to
    // plugins. Use DOM event mechanism after WebKit is made aware about
    // plugins that support composition.
    // The code below mimics the behavior of WebCore::Editor::setComposition.

    // Empty -> nonempty: composition started.
    if (composition_text_.empty() && !text.empty())
      focused_plugin_->HandleCompositionStart(string16());
    // Nonempty -> empty: composition canceled.
    if (!composition_text_.empty() && text.empty())
       focused_plugin_->HandleCompositionEnd(string16());
    composition_text_ = text;
    // Nonempty: composition is ongoing.
    if (!composition_text_.empty()) {
      focused_plugin_->HandleCompositionUpdate(composition_text_, underlines,
                                               selection_start, selection_end);
    }
  }
}

void PepperPluginDelegateImpl::OnImeConfirmComposition(const string16& text) {
  // Here, text.empty() has a special meaning. It means to commit the last
  // update of composition text (see RenderWidgetHost::ImeConfirmComposition()).
  const string16& last_text = text.empty() ? composition_text_ : text;

  // last_text is empty only when both text and composition_text_ is. Ignore it.
  if (last_text.empty())
    return;

  if (!IsPluginAcceptingCompositionEvents()) {
    for (size_t i = 0; i < text.size(); ++i) {
      WebKit::WebKeyboardEvent char_event;
      char_event.type = WebKit::WebInputEvent::Char;
      char_event.timeStampSeconds = base::Time::Now().ToDoubleT();
      char_event.modifiers = 0;
      char_event.windowsKeyCode = last_text[i];
      char_event.nativeKeyCode = last_text[i];
      char_event.text[0] = last_text[i];
      char_event.unmodifiedText[0] = last_text[i];
      if (render_view_->webwidget())
        render_view_->webwidget()->handleInputEvent(char_event);
    }
  } else {
    // Mimics the order of events sent by WebKit.
    // See WebCore::Editor::setComposition() for the corresponding code.
    focused_plugin_->HandleCompositionEnd(last_text);
    focused_plugin_->HandleTextInput(last_text);
  }
  composition_text_.clear();
}

gfx::Rect PepperPluginDelegateImpl::GetCaretBounds() const {
  if (!focused_plugin_)
    return gfx::Rect(0, 0, 0, 0);
  return focused_plugin_->GetCaretBounds();
}

ui::TextInputType PepperPluginDelegateImpl::GetTextInputType() const {
  if (!focused_plugin_)
    return ui::TEXT_INPUT_TYPE_NONE;
  return focused_plugin_->text_input_type();
}

void PepperPluginDelegateImpl::GetSurroundingText(string16* text,
                                                  ui::Range* range) const {
  if (!focused_plugin_)
    return;
  return focused_plugin_->GetSurroundingText(text, range);
}

bool PepperPluginDelegateImpl::IsPluginAcceptingCompositionEvents() const {
  if (!focused_plugin_)
    return false;
  return focused_plugin_->IsPluginAcceptingCompositionEvents();
}

bool PepperPluginDelegateImpl::CanComposeInline() const {
  return IsPluginAcceptingCompositionEvents();
}

void PepperPluginDelegateImpl::PluginCrashed(
    PepperPluginInstanceImpl* instance) {
  render_view_->PluginCrashed(instance->module()->path(),
                              instance->module()->GetPeerProcessId());
  UnSetAndDeleteLockTargetAdapter(instance);
}

void PepperPluginDelegateImpl::InstanceCreated(
    PepperPluginInstanceImpl* instance) {
  active_instances_.insert(instance);

  // Set the initial focus.
  instance->SetContentAreaFocus(render_view_->has_focus());
}

void PepperPluginDelegateImpl::InstanceDeleted(
    PepperPluginInstanceImpl* instance) {
  active_instances_.erase(instance);
  UnSetAndDeleteLockTargetAdapter(instance);

  if (last_mouse_event_target_ == instance)
    last_mouse_event_target_ = NULL;
  if (focused_plugin_ == instance)
    PluginFocusChanged(instance, false);
}

scoped_ptr< ::ppapi::thunk::ResourceCreationAPI>
PepperPluginDelegateImpl::CreateResourceCreationAPI(
    PepperPluginInstanceImpl* instance) {
  RendererPpapiHostImpl* host_impl = static_cast<RendererPpapiHostImpl*>(
      instance->module()->GetEmbedderState());
  return host_impl->CreateInProcessResourceCreationAPI(instance);
}

SkBitmap* PepperPluginDelegateImpl::GetSadPluginBitmap() {
  return GetContentClient()->renderer()->GetSadPluginBitmap();
}

WebKit::WebPlugin* PepperPluginDelegateImpl::CreatePluginReplacement(
    const base::FilePath& file_path) {
  return GetContentClient()->renderer()->CreatePluginReplacement(
      render_view_, file_path);
}

PluginDelegate::PlatformVideoDecoder*
    PepperPluginDelegateImpl::CreateVideoDecoder(
        media::VideoDecodeAccelerator::Client* client,
        int32 command_buffer_route_id) {
  return new PlatformVideoDecoderImpl(client, command_buffer_route_id);
}

void PepperPluginDelegateImpl::NumberOfFindResultsChanged(int identifier,
                                                          int total,
                                                          bool final_result) {
  render_view_->reportFindInPageMatchCount(identifier, total, final_result);
}

void PepperPluginDelegateImpl::SelectedFindResultChanged(int identifier,
                                                         int index) {
  render_view_->reportFindInPageSelection(
      identifier, index + 1, WebKit::WebRect());
}

uint32_t PepperPluginDelegateImpl::GetAudioHardwareOutputSampleRate() {
  RenderThreadImpl* thread = RenderThreadImpl::current();
  return thread->GetAudioHardwareConfig()->GetOutputSampleRate();
}

uint32_t PepperPluginDelegateImpl::GetAudioHardwareOutputBufferSize() {
  RenderThreadImpl* thread = RenderThreadImpl::current();
  return thread->GetAudioHardwareConfig()->GetOutputBufferSize();
}

PluginDelegate::PlatformAudioOutput*
    PepperPluginDelegateImpl::CreateAudioOutput(
        uint32_t sample_rate,
        uint32_t sample_count,
        PluginDelegate::PlatformAudioOutputClient* client) {
  return PepperPlatformAudioOutputImpl::Create(
      static_cast<int>(sample_rate), static_cast<int>(sample_count),
      GetRoutingID(), client);
}

// If a broker has not already been created for this plugin, creates one.
PluginDelegate::Broker* PepperPluginDelegateImpl::ConnectToBroker(
    PPB_Broker_Impl* client) {
  DCHECK(client);

  PluginModule* plugin_module = ResourceHelper::GetPluginModule(client);
  if (!plugin_module)
    return NULL;

  scoped_refptr<PepperBrokerImpl> broker =
      static_cast<PepperBrokerImpl*>(plugin_module->GetBroker());
  if (!broker.get()) {
    broker = CreateBroker(plugin_module);
    if (!broker.get())
      return NULL;
  }

  int request_id = pending_permission_requests_.Add(
      new base::WeakPtr<PPB_Broker_Impl>(client->AsWeakPtr()));
  if (!render_view_->Send(
          new ViewHostMsg_RequestPpapiBrokerPermission(
              render_view_->routing_id(),
              request_id,
              client->GetDocumentUrl(),
              plugin_module->path()))) {
    return NULL;
  }

  // Adds a reference, ensuring that the broker is not deleted when
  // |broker| goes out of scope.
  broker->AddPendingConnect(client);

  return broker.get();
}

void PepperPluginDelegateImpl::OnPpapiBrokerPermissionResult(
    int request_id,
    bool result) {
  scoped_ptr<base::WeakPtr<PPB_Broker_Impl> > client_ptr(
      pending_permission_requests_.Lookup(request_id));
  DCHECK(client_ptr.get());
  pending_permission_requests_.Remove(request_id);
  base::WeakPtr<PPB_Broker_Impl> client = *client_ptr;
  if (!client.get())
    return;

  PluginModule* plugin_module = ResourceHelper::GetPluginModule(client.get());
  if (!plugin_module)
    return;

  PepperBrokerImpl* broker =
      static_cast<PepperBrokerImpl*>(plugin_module->GetBroker());
  broker->OnBrokerPermissionResult(client.get(), result);
}

bool PepperPluginDelegateImpl::AsyncOpenFile(
    const base::FilePath& path,
    int flags,
    const AsyncOpenFileCallback& callback) {
  int message_id = pending_async_open_files_.Add(
      new AsyncOpenFileCallback(callback));
  IPC::Message* msg = new ViewHostMsg_AsyncOpenFile(
      render_view_->routing_id(), path, flags, message_id);
  return render_view_->Send(msg);
}

void PepperPluginDelegateImpl::OnAsyncFileOpened(
    base::PlatformFileError error_code,
    base::PlatformFile file,
    int message_id) {
  AsyncOpenFileCallback* callback =
      pending_async_open_files_.Lookup(message_id);
  DCHECK(callback);
  pending_async_open_files_.Remove(message_id);
  callback->Run(error_code, base::PassPlatformFile(&file));
  // Make sure we won't leak file handle if the requester has died.
  if (file != base::kInvalidPlatformFileValue)
    base::FileUtilProxy::Close(GetFileThreadMessageLoopProxy().get(),
                               file,
                               base::FileUtilProxy::StatusCallback());
  delete callback;
}

void PepperPluginDelegateImpl::OnSetFocus(bool has_focus) {
  for (std::set<PepperPluginInstanceImpl*>::iterator i =
           active_instances_.begin();
       i != active_instances_.end(); ++i)
    (*i)->SetContentAreaFocus(has_focus);
}

void PepperPluginDelegateImpl::PageVisibilityChanged(bool is_visible) {
  for (std::set<PepperPluginInstanceImpl*>::iterator i =
           active_instances_.begin();
       i != active_instances_.end(); ++i)
    (*i)->PageVisibilityChanged(is_visible);
}

bool PepperPluginDelegateImpl::IsPluginFocused() const {
  return focused_plugin_ != NULL;
}

void PepperPluginDelegateImpl::WillHandleMouseEvent() {
  // This method is called for every mouse event that the render view receives.
  // And then the mouse event is forwarded to WebKit, which dispatches it to the
  // event target. Potentially a Pepper plugin will receive the event.
  // In order to tell whether a plugin gets the last mouse event and which it
  // is, we set |last_mouse_event_target_| to NULL here. If a plugin gets the
  // event, it will notify us via DidReceiveMouseEvent() and set itself as
  // |last_mouse_event_target_|.
  last_mouse_event_target_ = NULL;
}

bool PepperPluginDelegateImpl::IsFileSystemOpened(PP_Instance instance,
                                                  PP_Resource resource) const {
  ppapi::host::ResourceHost* host = GetRendererResourceHost(instance, resource);
  if (!host)
    return false;
  PepperFileSystemHost* fs_host = host->AsPepperFileSystemHost();
  return fs_host && fs_host->IsOpened();
}

PP_FileSystemType PepperPluginDelegateImpl::GetFileSystemType(
    PP_Instance instance, PP_Resource resource) const {
  ppapi::host::ResourceHost* host = GetRendererResourceHost(instance, resource);
  if (!host)
    return PP_FILESYSTEMTYPE_INVALID;
  PepperFileSystemHost* fs_host = host->AsPepperFileSystemHost();
  return fs_host ? fs_host->GetType() : PP_FILESYSTEMTYPE_INVALID;
}

GURL PepperPluginDelegateImpl::GetFileSystemRootUrl(
    PP_Instance instance, PP_Resource resource) const {
  ppapi::host::ResourceHost* host = GetRendererResourceHost(instance, resource);
  if (!host)
    return GURL();
  PepperFileSystemHost* fs_host = host->AsPepperFileSystemHost();
  return fs_host ? fs_host->GetRootUrl() : GURL();
}

void PepperPluginDelegateImpl::MakeDirectory(
    const GURL& path,
    bool recursive,
    const StatusCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->CreateDirectory(
      path, false /* exclusive */, recursive, callback);
}

void PepperPluginDelegateImpl::Query(
    const GURL& path,
    const MetadataCallback& success_callback,
    const StatusCallback& error_callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->ReadMetadata(
      path, success_callback, error_callback);
}

void PepperPluginDelegateImpl::ReadDirectoryEntries(
    const GURL& path,
    const ReadDirectoryCallback& success_callback,
    const StatusCallback& error_callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->ReadDirectory(
      path, success_callback, error_callback);
}

void PepperPluginDelegateImpl::Touch(
    const GURL& path,
    const base::Time& last_access_time,
    const base::Time& last_modified_time,
    const StatusCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->TouchFile(path, last_access_time, last_modified_time,
                                    callback);
}

void PepperPluginDelegateImpl::SetLength(
    const GURL& path,
    int64_t length,
    const StatusCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->Truncate(path, length, NULL, callback);
}

void PepperPluginDelegateImpl::Delete(
    const GURL& path,
    const StatusCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->Remove(path, false /* recursive */, callback);
}

void PepperPluginDelegateImpl::Rename(
    const GURL& file_path,
    const GURL& new_file_path,
    const StatusCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->Move(file_path, new_file_path, callback);
}

void PepperPluginDelegateImpl::ReadDirectory(
    const GURL& directory_path,
    const ReadDirectoryCallback& success_callback,
    const StatusCallback& error_callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->ReadDirectory(
      directory_path, success_callback, error_callback);
}

void PepperPluginDelegateImpl::AsyncOpenFileSystemURL(
    const GURL& path,
    int flags,
    const AsyncOpenFileSystemURLCallback& callback) {
  FileSystemDispatcher* file_system_dispatcher =
      ChildThread::current()->file_system_dispatcher();
  file_system_dispatcher->OpenFile(
      path, flags,
      base::Bind(&DidOpenFileSystemURL, callback),
      base::Bind(&DidFailOpenFileSystemURL, callback));
}

void PepperPluginDelegateImpl::SyncGetFileSystemPlatformPath(
    const GURL& url, base::FilePath* platform_path) {
  RenderThreadImpl::current()->Send(new FileSystemHostMsg_SyncGetPlatformPath(
      url, platform_path));
}

scoped_refptr<base::MessageLoopProxy>
PepperPluginDelegateImpl::GetFileThreadMessageLoopProxy() {
  return RenderThreadImpl::current()->GetFileThreadMessageLoopProxy();
}

uint32 PepperPluginDelegateImpl::TCPSocketCreate() {
  // This was used for PPB_TCPSocket_Private creation. And it shouldn't be
  // needed anymore.
  // TODO(yzshen): Remove TCP socket-related contents from the plugin delegate.
  uint32 socket_id = 0;
  render_view_->Send(new PpapiHostMsg_PPBTCPSocket_CreatePrivate(
      render_view_->routing_id(), 0, &socket_id));
  return socket_id;
}

void PepperPluginDelegateImpl::TCPSocketConnect(
    PPB_TCPSocket_Private_Impl* socket,
    uint32 socket_id,
    const std::string& host,
    uint16_t port) {
  RegisterTCPSocket(socket, socket_id);
  render_view_->Send(
      new PpapiHostMsg_PPBTCPSocket_Connect(
          render_view_->routing_id(), socket_id, host, port));
}

void PepperPluginDelegateImpl::TCPSocketConnectWithNetAddress(
      PPB_TCPSocket_Private_Impl* socket,
      uint32 socket_id,
      const PP_NetAddress_Private& addr) {
  RegisterTCPSocket(socket, socket_id);
  render_view_->Send(
      new PpapiHostMsg_PPBTCPSocket_ConnectWithNetAddress(
          render_view_->routing_id(), socket_id, addr));
}

void PepperPluginDelegateImpl::TCPSocketSSLHandshake(
    uint32 socket_id,
    const std::string& server_name,
    uint16_t server_port,
    const std::vector<std::vector<char> >& trusted_certs,
    const std::vector<std::vector<char> >& untrusted_certs) {
  DCHECK(tcp_sockets_.Lookup(socket_id));
  render_view_->Send(new PpapiHostMsg_PPBTCPSocket_SSLHandshake(
      socket_id, server_name, server_port, trusted_certs, untrusted_certs));
}

void PepperPluginDelegateImpl::TCPSocketRead(uint32 socket_id,
                                             int32_t bytes_to_read) {
  DCHECK(tcp_sockets_.Lookup(socket_id));
  render_view_->Send(
      new PpapiHostMsg_PPBTCPSocket_Read(socket_id, bytes_to_read));
}

void PepperPluginDelegateImpl::TCPSocketWrite(uint32 socket_id,
                                              const std::string& buffer) {
  DCHECK(tcp_sockets_.Lookup(socket_id));
  render_view_->Send(new PpapiHostMsg_PPBTCPSocket_Write(socket_id, buffer));
}

void PepperPluginDelegateImpl::TCPSocketDisconnect(uint32 socket_id) {
  // There is no DCHECK(tcp_sockets_.Lookup(socket_id)) because this method
  // can be called before TCPSocketConnect or TCPSocketConnectWithNetAddress.
  render_view_->Send(new PpapiHostMsg_PPBTCPSocket_Disconnect(socket_id));
  if (tcp_sockets_.Lookup(socket_id))
    tcp_sockets_.Remove(socket_id);
}

void PepperPluginDelegateImpl::TCPSocketSetOption(
    uint32 socket_id,
    PP_TCPSocket_Option name,
    const ppapi::SocketOptionData& value) {
  DCHECK(tcp_sockets_.Lookup(socket_id));
  render_view_->Send(
      new PpapiHostMsg_PPBTCPSocket_SetOption(socket_id, name, value));
}

void PepperPluginDelegateImpl::RegisterTCPSocket(
    PPB_TCPSocket_Private_Impl* socket,
    uint32 socket_id) {
  tcp_sockets_.AddWithID(socket, socket_id);
}

void PepperPluginDelegateImpl::TCPServerSocketListen(
    PP_Resource socket_resource,
    const PP_NetAddress_Private& addr,
    int32_t backlog) {
  render_view_->Send(
      new PpapiHostMsg_PPBTCPServerSocket_Listen(
          render_view_->routing_id(), 0, socket_resource, addr, backlog));
}

void PepperPluginDelegateImpl::TCPServerSocketAccept(uint32 server_socket_id) {
  DCHECK(tcp_server_sockets_.Lookup(server_socket_id));
  render_view_->Send(new PpapiHostMsg_PPBTCPServerSocket_Accept(
      render_view_->routing_id(), server_socket_id));
}

void PepperPluginDelegateImpl::TCPServerSocketStopListening(
    PP_Resource socket_resource,
    uint32 socket_id) {
  if (socket_id != 0) {
    render_view_->Send(new PpapiHostMsg_PPBTCPServerSocket_Destroy(socket_id));
    tcp_server_sockets_.Remove(socket_id);
  }
}

bool PepperPluginDelegateImpl::X509CertificateParseDER(
    const std::vector<char>& der,
    ppapi::PPB_X509Certificate_Fields* fields) {
  bool succeeded = false;
  render_view_->Send(
      new PpapiHostMsg_PPBX509Certificate_ParseDER(der, &succeeded, fields));
  return succeeded;
}

FullscreenContainer* PepperPluginDelegateImpl::CreateFullscreenContainer(
    PepperPluginInstanceImpl* instance) {
  return render_view_->CreatePepperFullscreenContainer(instance);
}

gfx::Size PepperPluginDelegateImpl::GetScreenSize() {
  WebKit::WebScreenInfo info = render_view_->screenInfo();
  return gfx::Size(info.rect.width, info.rect.height);
}

std::string PepperPluginDelegateImpl::GetDefaultEncoding() {
  return render_view_->webkit_preferences().default_encoding;
}

void PepperPluginDelegateImpl::ZoomLimitsChanged(double minimum_factor,
                                                 double maximum_factor) {
  double minimum_level = ZoomFactorToZoomLevel(minimum_factor);
  double maximum_level = ZoomFactorToZoomLevel(maximum_factor);
  render_view_->webview()->zoomLimitsChanged(minimum_level, maximum_level);
}

void PepperPluginDelegateImpl::HandleDocumentLoad(
    PepperPluginInstanceImpl* instance,
    const WebKit::WebURLResponse& response) {
  DCHECK(!instance->document_loader());

  PP_Instance pp_instance = instance->pp_instance();
  RendererPpapiHostImpl* host_impl = static_cast<RendererPpapiHostImpl*>(
      instance->module()->GetEmbedderState());

  // Create a loader resource host for this load. Note that we have to set
  // the document_loader before issuing the in-process
  // PPP_Instance.HandleDocumentLoad call below, since this may reentrantly
  // call into the instance and expect it to be valid.
  PepperURLLoaderHost* loader_host =
      new PepperURLLoaderHost(host_impl, true, pp_instance, 0);
  instance->set_document_loader(loader_host);
  loader_host->didReceiveResponse(NULL, response);

  // This host will be pending until the resource object attaches to it.
  int pending_host_id = host_impl->GetPpapiHost()->AddPendingResourceHost(
      scoped_ptr<ppapi::host::ResourceHost>(loader_host));
  DCHECK(pending_host_id);
  ppapi::URLResponseInfoData data =
      DataFromWebURLResponse(pp_instance, response);

  if (host_impl->in_process_router()) {
    // Running in-process, we can just create the resource and call the
    // PPP_Instance function directly.
    scoped_refptr<ppapi::proxy::URLLoaderResource> loader_resource(
        new ppapi::proxy::URLLoaderResource(
            host_impl->in_process_router()->GetPluginConnection(pp_instance),
            pp_instance, pending_host_id, data));

    PP_Resource loader_pp_resource = loader_resource->GetReference();
    if (!instance->instance_interface()->HandleDocumentLoad(
            instance->pp_instance(), loader_pp_resource))
      loader_resource->Close();
    // We don't pass a ref into the plugin, if it wants one, it will have taken
    // an additional one.
    ppapi::PpapiGlobals::Get()->GetResourceTracker()->ReleaseResource(
        loader_pp_resource);

    // Danger! If the plugin doesn't take a ref in HandleDocumentLoad, the
    // resource host will be destroyed as soon as our scoped_refptr for the
    // resource goes out of scope.
    //
    // Null it out so people don't accidentally add code below that uses it.
    loader_host = NULL;
  } else {
    // Running out-of-process. Initiate an IPC call to notify the plugin
    // process.
    ppapi::proxy::HostDispatcher* dispatcher =
        ppapi::proxy::HostDispatcher::GetForInstance(pp_instance);
    dispatcher->Send(new PpapiMsg_PPPInstance_HandleDocumentLoad(
        ppapi::API_ID_PPP_INSTANCE, pp_instance, pending_host_id, data));
  }
}

RendererPpapiHost* PepperPluginDelegateImpl::CreateExternalPluginModule(
    scoped_refptr<PluginModule> module,
    const base::FilePath& path,
    ppapi::PpapiPermissions permissions,
    const IPC::ChannelHandle& channel_handle,
    base::ProcessId peer_pid,
    int plugin_child_id) {
  // We don't call PepperPluginRegistry::AddLiveModule, as this module is
  // managed externally.
  return CreateOutOfProcessModule(module.get(),
                                  path,
                                  permissions,
                                  channel_handle,
                                  peer_pid,
                                  plugin_child_id,
                                  true);  // is_external = true
}

base::SharedMemory* PepperPluginDelegateImpl::CreateAnonymousSharedMemory(
    size_t size) {
  return RenderThread::Get()->HostAllocateSharedMemoryBuffer(size).release();
}

ppapi::Preferences PepperPluginDelegateImpl::GetPreferences() {
  return ppapi::Preferences(render_view_->webkit_preferences());
}

bool PepperPluginDelegateImpl::LockMouse(PepperPluginInstanceImpl* instance) {
  return GetMouseLockDispatcher(instance)->LockMouse(
      GetOrCreateLockTargetAdapter(instance));
}

void PepperPluginDelegateImpl::UnlockMouse(PepperPluginInstanceImpl* instance) {
  GetMouseLockDispatcher(instance)->UnlockMouse(
      GetOrCreateLockTargetAdapter(instance));
}

bool PepperPluginDelegateImpl::IsMouseLocked(
    PepperPluginInstanceImpl* instance) {
  return GetMouseLockDispatcher(instance)->IsMouseLockedTo(
      GetOrCreateLockTargetAdapter(instance));
}

void PepperPluginDelegateImpl::DidChangeCursor(
    PepperPluginInstanceImpl* instance,
    const WebKit::WebCursorInfo& cursor) {
  // Update the cursor appearance immediately if the requesting plugin is the
  // one which receives the last mouse event. Otherwise, the new cursor won't be
  // picked up until the plugin gets the next input event. That is bad if, e.g.,
  // the plugin would like to set an invisible cursor when there isn't any user
  // input for a while.
  if (instance == last_mouse_event_target_)
    render_view_->didChangeCursor(cursor);
}

void PepperPluginDelegateImpl::DidReceiveMouseEvent(
    PepperPluginInstanceImpl* instance) {
  last_mouse_event_target_ = instance;
}

bool PepperPluginDelegateImpl::IsInFullscreenMode() {
  return render_view_->is_fullscreen();
}

void PepperPluginDelegateImpl::SampleGamepads(WebKit::WebGamepads* data) {
  if (!gamepad_shared_memory_reader_)
    gamepad_shared_memory_reader_.reset(new GamepadSharedMemoryReader);
  gamepad_shared_memory_reader_->SampleGamepads(*data);
}

bool PepperPluginDelegateImpl::IsPageVisible() const {
  return !render_view_->is_hidden();
}

bool PepperPluginDelegateImpl::OnMessageReceived(const IPC::Message& message) {
  if (pepper_browser_connection_.OnMessageReceived(message))
    return true;

  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(PepperPluginDelegateImpl, message)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPSocket_ConnectACK,
                        OnTCPSocketConnectACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPSocket_SSLHandshakeACK,
                        OnTCPSocketSSLHandshakeACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPSocket_ReadACK, OnTCPSocketReadACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPSocket_WriteACK, OnTCPSocketWriteACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPSocket_SetOptionACK,
                        OnTCPSocketSetOptionACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPServerSocket_ListenACK,
                        OnTCPServerSocketListenACK)
    IPC_MESSAGE_HANDLER(PpapiMsg_PPBTCPServerSocket_AcceptACK,
                        OnTCPServerSocketAcceptACK)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void PepperPluginDelegateImpl::OnDestruct() {
  // Nothing to do here. Default implementation in RenderViewObserver does
  // 'delete this' but it's not suitable for PepperPluginDelegateImpl because
  // it's non-pointer member in RenderViewImpl.
}

void PepperPluginDelegateImpl::OnTCPSocketConnectACK(
    uint32 plugin_dispatcher_id,
    uint32 socket_id,
    int32_t result,
    const PP_NetAddress_Private& local_addr,
    const PP_NetAddress_Private& remote_addr) {
  PPB_TCPSocket_Private_Impl* socket = tcp_sockets_.Lookup(socket_id);
  if (socket)
    socket->OnConnectCompleted(result, local_addr, remote_addr);
  if (result != PP_OK)
    tcp_sockets_.Remove(socket_id);
}

void PepperPluginDelegateImpl::OnTCPSocketSSLHandshakeACK(
    uint32 plugin_dispatcher_id,
    uint32 socket_id,
    bool succeeded,
    const ppapi::PPB_X509Certificate_Fields& certificate_fields) {
  PPB_TCPSocket_Private_Impl* socket = tcp_sockets_.Lookup(socket_id);
  if (socket)
    socket->OnSSLHandshakeCompleted(succeeded, certificate_fields);
}

void PepperPluginDelegateImpl::OnTCPSocketReadACK(uint32 plugin_dispatcher_id,
                                                  uint32 socket_id,
                                                  int32_t result,
                                                  const std::string& data) {
  PPB_TCPSocket_Private_Impl* socket = tcp_sockets_.Lookup(socket_id);
  if (socket)
    socket->OnReadCompleted(result, data);
}

void PepperPluginDelegateImpl::OnTCPSocketWriteACK(uint32 plugin_dispatcher_id,
                                                   uint32 socket_id,
                                                   int32_t result) {
  PPB_TCPSocket_Private_Impl* socket = tcp_sockets_.Lookup(socket_id);
  if (socket)
    socket->OnWriteCompleted(result);
}

void PepperPluginDelegateImpl::OnTCPSocketSetOptionACK(
    uint32 plugin_dispatcher_id,
    uint32 socket_id,
    int32_t result) {
  PPB_TCPSocket_Private_Impl* socket = tcp_sockets_.Lookup(socket_id);
  if (socket)
    socket->OnSetOptionCompleted(result);
}

void PepperPluginDelegateImpl::OnTCPServerSocketListenACK(
    uint32 plugin_dispatcher_id,
    PP_Resource socket_resource,
    uint32 socket_id,
    const PP_NetAddress_Private& local_addr,
    int32_t status) {
  ppapi::thunk::EnterResource<ppapi::thunk::PPB_TCPServerSocket_Private_API>
      enter(socket_resource, true);
  if (enter.succeeded()) {
    ppapi::PPB_TCPServerSocket_Shared* socket =
        static_cast<ppapi::PPB_TCPServerSocket_Shared*>(enter.object());
    if (status == PP_OK)
      tcp_server_sockets_.AddWithID(socket, socket_id);
    socket->OnListenCompleted(socket_id, local_addr, status);
  } else if (socket_id != 0 && status == PP_OK) {
    // StopListening was called before completion of Listen.
    render_view_->Send(new PpapiHostMsg_PPBTCPServerSocket_Destroy(socket_id));
  }
}

void PepperPluginDelegateImpl::OnTCPServerSocketAcceptACK(
    uint32 plugin_dispatcher_id,
    uint32 server_socket_id,
    uint32 accepted_socket_id,
    const PP_NetAddress_Private& local_addr,
    const PP_NetAddress_Private& remote_addr) {
  ppapi::PPB_TCPServerSocket_Shared* socket =
      tcp_server_sockets_.Lookup(server_socket_id);
  if (socket) {
    bool succeeded = (accepted_socket_id != 0);
    socket->OnAcceptCompleted(succeeded,
                              accepted_socket_id,
                              local_addr,
                              remote_addr);
  } else if (accepted_socket_id != 0) {
    render_view_->Send(
        new PpapiHostMsg_PPBTCPSocket_Disconnect(accepted_socket_id));
  }
}

int PepperPluginDelegateImpl::GetRoutingID() const {
  return render_view_->routing_id();
}

MouseLockDispatcher::LockTarget*
    PepperPluginDelegateImpl::GetOrCreateLockTargetAdapter(
    PepperPluginInstanceImpl* instance) {
  MouseLockDispatcher::LockTarget* target = mouse_lock_instances_[instance];
  if (target)
    return target;

  return mouse_lock_instances_[instance] =
      new PluginInstanceLockTarget(instance);
}

void PepperPluginDelegateImpl::UnSetAndDeleteLockTargetAdapter(
    PepperPluginInstanceImpl* instance) {
  LockTargetMap::iterator it = mouse_lock_instances_.find(instance);
  if (it != mouse_lock_instances_.end()) {
    MouseLockDispatcher::LockTarget* target = it->second;
    GetMouseLockDispatcher(instance)->OnLockTargetDestroyed(target);
    delete target;
    mouse_lock_instances_.erase(it);
  }
}

MouseLockDispatcher* PepperPluginDelegateImpl::GetMouseLockDispatcher(
    PepperPluginInstanceImpl* instance) {
  if (instance->flash_fullscreen()) {
    RenderWidgetFullscreenPepper* container =
        static_cast<RenderWidgetFullscreenPepper*>(
            instance->fullscreen_container());
    return container->mouse_lock_dispatcher();
  } else {
    return render_view_->mouse_lock_dispatcher();
  }
}

IPC::PlatformFileForTransit PepperPluginDelegateImpl::ShareHandleWithRemote(
    base::PlatformFile handle,
    base::ProcessId target_process_id,
    bool should_close_source) const {
  return BrokerGetFileHandleForProcess(
      handle,
      target_process_id,
      should_close_source);
}

bool PepperPluginDelegateImpl::IsRunningInProcess(PP_Instance instance) const {
  RendererPpapiHostImpl* host =
      RendererPpapiHostImpl::GetForPPInstance(instance);
  return host && host->IsRunningInProcess();
}

}  // namespace content
