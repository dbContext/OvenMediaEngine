#include <utility>

#include "rtc_private.h"
#include "rtc_session.h"
#include "rtc_stream.h"
#include "webrtc_publisher.h"

#include "config/config_manager.h"

#include <orchestrator/orchestrator.h>

std::shared_ptr<WebRtcPublisher> WebRtcPublisher::Create(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router)
{
	auto webrtc = std::make_shared<WebRtcPublisher>(server_config, router);

	if (!webrtc->Start())
	{
		logte("An error occurred while creating WebRtcPublisher");
		return nullptr;
	}
	return webrtc;
}

WebRtcPublisher::WebRtcPublisher(const cfg::Server &server_config, const std::shared_ptr<MediaRouteInterface> &router)
	: Publisher(server_config, router)
{
}

WebRtcPublisher::~WebRtcPublisher()
{
	logtd("WebRtcPublisher has been terminated finally");
}

/*
 * Publisher Implementation
 */

bool WebRtcPublisher::Start()
{
	auto server_config = GetServerConfig();
	auto webrtc_bind_config = server_config.GetBind().GetPublishers().GetWebrtc();

	if (webrtc_bind_config.IsParsed() == false)
	{
		logtw("%s is disabled by configuration", GetPublisherName());
		return true;
	}

	auto &signalling_config = webrtc_bind_config.GetSignalling();
	auto &port_config = signalling_config.GetPort();
	auto &tls_port_config = signalling_config.GetTlsPort();
	auto worker_count = signalling_config.GetWorker();

	auto port = static_cast<uint16_t>(port_config.GetPort());
	auto tls_port = static_cast<uint16_t>(tls_port_config.GetPort());
	bool has_port = (port != 0);
	bool has_tls_port = (tls_port != 0);

	if ((has_port == false) && (has_tls_port == false))
	{
		logte("Invalid WebRTC Port settings");
		return false;
	}

	ov::SocketAddress signalling_address = ov::SocketAddress(server_config.GetIp(), port);
	ov::SocketAddress signalling_tls_address = ov::SocketAddress(server_config.GetIp(), tls_port);

	// Initialize RtcSignallingServer
	_signalling_server = std::make_shared<RtcSignallingServer>(server_config);
	_signalling_server->AddObserver(RtcSignallingObserver::GetSharedPtr());
	if (_signalling_server->Start(has_port ? &signalling_address : nullptr, has_tls_port ? &signalling_tls_address : nullptr, worker_count) == false)
	{
		return false;
	}

	bool result = true;

	_ice_port = IcePortManager::GetInstance()->CreatePort(IcePortObserver::GetSharedPtr());
	if (_ice_port == nullptr)
	{
		logte("Could not initialize ICE Port. Check your ICE configuration");
		result = false;
	}

	if(IcePortManager::GetInstance()->CreateIceCandidates(_ice_port, webrtc_bind_config.GetIceCandidates()) == false)
	{
		logte("Could not create ICE Candidates. Check your ICE configuration");
		result = false;
	}

	if (result)
	{
		logti("%s is listening on %s%s%s%s...",
			  GetPublisherName(),
			  has_port ? signalling_address.ToString().CStr() : "",
			  (has_port && has_tls_port) ? ", " : "",
			  has_tls_port ? "TLS: " : "",
			  has_tls_port ? signalling_tls_address.ToString().CStr() : "");
	}
	else
	{
		// Rollback
		logte("An error occurred while initialize WebRTC Publisher. Stopping RtcSignallingServer...");

		_signalling_server->Stop();

		return false;
	}

	_message_thread.Start(ov::MessageThreadObserver<std::shared_ptr<ov::CommonMessage>>::GetSharedPtr());


	_timer.Push(
		[this](void *parameter) -> ov::DelayQueueAction 
		{
			// 2018-12-24 23:06:25.035,RTSP.SS,CONN_COUNT,INFO,,,[Live users], [Playback users]
			std::shared_ptr<info::Application> rtsp_live_app_info;
			std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
			std::shared_ptr<info::Application> rtsp_play_app_info;
			std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

			rtsp_live_app_metrics = nullptr;
			rtsp_play_app_metrics = nullptr;
			
			// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
			rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
			if (rtsp_live_app_info != nullptr)
			{
				rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
			}
			rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
			if (rtsp_play_app_info != nullptr)
			{
				rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
			}

			stat_log(STAT_LOG_WEBRTC_EDGE_VIEWERS, "%s,%s,%s,%s,,,%u,%u",
					ov::Clock::Now().CStr(),
					"WEBRTC.SS",
					"CONN_COUNT",
					"INFO",
					rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
					rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0);

			return ov::DelayQueueAction::Repeat;
		}
		, 1000);

	_timer.Start();

	return Publisher::Start();
}

bool WebRtcPublisher::Stop()
{
	IcePortManager::GetInstance()->ReleasePort(_ice_port, IcePortObserver::GetSharedPtr());

	_signalling_server->RemoveObserver(RtcSignallingObserver::GetSharedPtr());
	_signalling_server->Stop();

	_message_thread.Stop();

	return Publisher::Stop();
}

bool WebRtcPublisher::DisconnectSession(const std::shared_ptr<RtcSession> &session)
{
	auto message = std::make_shared<ov::CommonMessage>();
	message->_code = static_cast<uint32_t>(MessageCode::DISCONNECT_SESSION);
	message->_message = std::make_any<std::shared_ptr<RtcSession>>(session);

	_message_thread.PostMessage(message);

	return true;
}

bool WebRtcPublisher::DisconnectSessionInternal(const std::shared_ptr<RtcSession> &session)
{
	auto stream = std::dynamic_pointer_cast<RtcStream>(session->GetStream());

	stream->RemoveSession(session->GetId());
	auto stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(stream));
	if (stream_metrics != nullptr)
	{
		stream_metrics->OnSessionDisconnected(PublisherType::Webrtc);
	}

	session->Stop();

	// Special purpose log
	stat_log(STAT_LOG_WEBRTC_EDGE_SESSION, "%s,%s,%s,%s,,,%s,%s,%u",
					ov::Clock::Now().CStr(),
					"WEBRTC.SS",
					"SESSION",
					"INFO",
					"deleteClientSession",
					stream->GetName().CStr(),
					session->GetId());

	std::shared_ptr<info::Application> rtsp_live_app_info;
	std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
	std::shared_ptr<info::Application> rtsp_play_app_info;
	std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

	rtsp_live_app_metrics = nullptr;
	rtsp_play_app_metrics = nullptr;

	// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
	rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
	if (rtsp_live_app_info != nullptr)
	{
		rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
	}
	rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
	if (rtsp_play_app_info != nullptr)
	{
		rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
	}

	stat_log(STAT_LOG_WEBRTC_EDGE_SESSION, "%s,%s,%s,%s,,,%s:%d,%s:%d,%s,%u",
				ov::Clock::Now().CStr(),
				"WEBRTC.SS",
				"SESSION",
				"INFO",
				"Live",
				rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
				"Playback",
				rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0,
				stream->GetName().CStr(),
				session->GetId());

	return true;
}

void WebRtcPublisher::OnMessage(const std::shared_ptr<ov::CommonMessage> &message)
{
	auto code = static_cast<MessageCode>(message->_code);
	if(code == MessageCode::DISCONNECT_SESSION)
	{
		try 
		{
			auto session = std::any_cast<std::shared_ptr<RtcSession>>(message->_message);
			if(session == nullptr)
			{
				return;
			}

			_ice_port->RemoveSession(session);
			DisconnectSessionInternal(session);
		}
		catch(const std::bad_any_cast& e) 
		{
			logtc("Wrong message!");
			return;
		}
	}
}

std::shared_ptr<pub::Application> WebRtcPublisher::OnCreatePublisherApplication(const info::Application &application_info)
{
	return RtcApplication::Create(pub::Publisher::GetSharedPtrAs<pub::Publisher>(), application_info, _ice_port, _signalling_server);
}

bool WebRtcPublisher::OnDeletePublisherApplication(const std::shared_ptr<pub::Application> &application)
{
	return true;
}

/*
 * Signalling Implementation
 */

// Called when receives request offer sdp from client
std::shared_ptr<const SessionDescription> WebRtcPublisher::OnRequestOffer(const std::shared_ptr<WebSocketClient> &ws_client,
																		  const info::VHostAppName &vhost_app_name, const ov::String &host_name, const ov::String &stream_name,
																		  std::vector<RtcIceCandidate> *ice_candidates)
{
	[[maybe_unused]] RequestStreamResult result = RequestStreamResult::init;
	auto request = ws_client->GetClient()->GetRequest();
	auto remote_address = request->GetRemote()->GetRemoteAddress();
	auto uri = request->GetUri();
	auto parsed_url = ov::Url::Parse(uri);

	if (parsed_url == nullptr)
	{
		logte("Could not parse the url: %s", uri.CStr());
		return nullptr;
	}

	std::shared_ptr<const SignedPolicy> signed_policy;
	std::shared_ptr<const SignedToken> signed_token;
	auto signed_policy_result = Publisher::HandleSignedPolicy(parsed_url, remote_address, signed_policy);
	if(signed_policy_result == CheckSignatureResult::Error)
	{
		return nullptr;
	}
	else if(signed_policy_result == CheckSignatureResult::Fail)
	{
		logtw("%s", signed_policy->GetErrMessage().CStr());
		return nullptr;
	}
	else if(signed_policy_result == CheckSignatureResult::Off)
	{
		// SingedToken
		auto signed_token_result = Publisher::HandleSignedToken(parsed_url, remote_address, signed_token);
		if(signed_token_result == CheckSignatureResult::Error)
		{
			return nullptr;
		}
		else if(signed_token_result == CheckSignatureResult::Fail)
		{
			logtw("%s", signed_token->GetErrMessage().CStr());
			return nullptr;
		}
	}

	auto stream = std::static_pointer_cast<RtcStream>(GetStream(vhost_app_name, stream_name));
	if(stream == nullptr)
	{
		stream = std::dynamic_pointer_cast<RtcStream>(PullStream(parsed_url, vhost_app_name, host_name, stream_name));
		if(stream == nullptr)
		{
			result = RequestStreamResult::origin_failed;
		}
		else
		{
			// Connection Request log
			// 2019-11-06 09:46:45.390 , RTSP.SS ,REQUEST,INFO,,,Live,rtsp://50.1.111.154:10915/1135/1/,220.103.225.254_44757_1573001205_389304_128855562
			stat_log(STAT_LOG_WEBRTC_EDGE_REQUEST, "%s,%s,%s,%s,,,%s,%s,%s",
						ov::Clock::Now().CStr(),
						"WEBRTC.SS",
						"REQUEST",
						"INFO",
						vhost_app_name.CStr(),
						stream->GetMediaSource().CStr(),
						remote_address->ToString().CStr());

			logti("URL %s is requested", stream->GetMediaSource().CStr());
		}
	}
	else
	{
		result = RequestStreamResult::local_success;
	}

	if (stream == nullptr)
	{
		logte("Cannot find stream (%s/%s)", vhost_app_name.CStr(), stream_name.CStr());
		return nullptr;
	}

	if(stream->WaitUntilStart(3000) == false)
	{
		logtw("(%s/%s) stream has not started.", vhost_app_name.CStr(), stream_name.CStr());
		return nullptr;
	}

	auto &candidates = _ice_port->GetIceCandidateList();
	ice_candidates->insert(ice_candidates->end(), candidates.cbegin(), candidates.cend());
	auto session_description = std::make_shared<SessionDescription>(*stream->GetSessionDescription());
	session_description->SetOrigin("OvenMediaEngine", ++_last_issued_session_id, 2, "IN", 4, "127.0.0.1");
	session_description->SetIceUfrag(_ice_port->GenerateUfrag());
	session_description->Update();

	return session_description;
}

// Called when receives an answer sdp from client
bool WebRtcPublisher::OnAddRemoteDescription(const std::shared_ptr<WebSocketClient> &ws_client,
											 const info::VHostAppName &vhost_app_name, const ov::String &host_name, const ov::String &stream_name,
											 const std::shared_ptr<const SessionDescription> &offer_sdp,
											 const std::shared_ptr<const SessionDescription> &peer_sdp)
{
	auto application = GetApplicationByName(vhost_app_name);
	auto stream = GetStream(vhost_app_name, stream_name);
	if (!stream)
	{
		logte("Cannot find stream (%s/%s)", vhost_app_name.CStr(), stream_name.CStr());
		return false;
	}

	// SignedPolicy and SignedToken
	auto request = ws_client->GetClient()->GetRequest();
	auto remote_address = request->GetRemote()->GetRemoteAddress();
	auto uri = request->GetUri();
	auto parsed_url = ov::Url::Parse(uri);

	if (parsed_url == nullptr)
	{
		logte("Could not parse the url: %s", uri.CStr());
		return false;
	}

	uint64_t session_expired_time = 0;
	std::shared_ptr<const SignedPolicy> signed_policy;
	std::shared_ptr<const SignedToken> signed_token;
	auto signed_policy_result = Publisher::HandleSignedPolicy(parsed_url, remote_address, signed_policy);
	if(signed_policy_result == CheckSignatureResult::Error)
	{
		return false;
	}
	else if(signed_policy_result == CheckSignatureResult::Fail)
	{
		logtw("%s", signed_policy->GetErrMessage().CStr());
		return false;
	}
	else if(signed_policy_result == CheckSignatureResult::Pass)
	{
		session_expired_time = signed_policy->GetStreamExpireEpochSec();
	}
	else if(signed_policy_result == CheckSignatureResult::Off)
	{
		// SingedToken
		auto signed_token_result = Publisher::HandleSignedToken(parsed_url, remote_address, signed_token);
		if(signed_token_result == CheckSignatureResult::Error)
		{
			return false;
		}
		else if(signed_token_result == CheckSignatureResult::Fail)
		{
			logtw("%s", signed_token->GetErrMessage().CStr());
			return false;
		}
		else if(signed_token_result == CheckSignatureResult::Pass)
		{
			session_expired_time = signed_token->GetStreamExpiredTime();
		}
	}

	ov::String remote_sdp_text = peer_sdp->ToString();
	logtd("OnAddRemoteDescription: %s", remote_sdp_text.CStr());

	auto session = RtcSession::Create(Publisher::GetSharedPtrAs<WebRtcPublisher>(), application, stream, offer_sdp, peer_sdp, _ice_port, ws_client);
	if (session != nullptr)
	{
		if(session_expired_time != 0)
		{
			session->SetSessionExpiredTime(session_expired_time);
		}

		stream->AddSession(session);
		auto stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(stream));
		if (stream_metrics != nullptr)
		{
			stream_metrics->OnSessionConnected(PublisherType::Webrtc);
		}

		_ice_port->AddSession(session, offer_sdp, peer_sdp);

		// Session is created

		// Special purpose log
		stat_log(STAT_LOG_WEBRTC_EDGE_SESSION, "%s,%s,%s,%s,,,%s,%s,%u",
					 ov::Clock::Now().CStr(),
					 "WEBRTC.SS",
					 "SESSION",
					 "INFO",
					 "createClientSession",
					 stream->GetName().CStr(),
					 session->GetId());

		std::shared_ptr<info::Application> rtsp_live_app_info;
		std::shared_ptr<mon::ApplicationMetrics> rtsp_live_app_metrics;
		std::shared_ptr<info::Application> rtsp_play_app_info;
		std::shared_ptr<mon::ApplicationMetrics> rtsp_play_app_metrics;

		rtsp_live_app_metrics = nullptr;
		rtsp_play_app_metrics = nullptr;

		// This log only for the "default" host and the "rtsp_live"/"rtsp_playback" applications 
		rtsp_live_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_live")));
		if (rtsp_live_app_info != nullptr)
		{
			rtsp_live_app_metrics = ApplicationMetrics(*rtsp_live_app_info);
		}
		rtsp_play_app_info = std::static_pointer_cast<info::Application>(GetApplicationByName(ocst::Orchestrator::GetInstance()->ResolveApplicationName("default", "rtsp_playback")));
		if (rtsp_play_app_info != nullptr)
		{
			rtsp_play_app_metrics = ApplicationMetrics(*rtsp_play_app_info);
		}

		stat_log(STAT_LOG_WEBRTC_EDGE_SESSION, "%s,%s,%s,%s,,,%s:%d,%s:%d,%s,%u",
					ov::Clock::Now().CStr(),
					"WEBRTC.SS",
					"SESSION",
					"INFO",
					"Live",
					rtsp_live_app_metrics != nullptr ? rtsp_live_app_metrics->GetTotalConnections() : 0,
					"Playback",
					rtsp_play_app_metrics != nullptr ? rtsp_play_app_metrics->GetTotalConnections() : 0,
					stream->GetName().CStr(),
					session->GetId());
	}
	else
	{
		logte("Cannot create session");
	}

	return true;
}

bool WebRtcPublisher::OnStopCommand(const std::shared_ptr<WebSocketClient> &ws_client,
									const info::VHostAppName &vhost_app_name, const ov::String &host_name, const ov::String &stream_name,
									const std::shared_ptr<const SessionDescription> &offer_sdp,
									const std::shared_ptr<const SessionDescription> &peer_sdp)
{
	logti("Stop commnad received : %s/%s/%u", vhost_app_name.CStr(), stream_name.CStr(), offer_sdp->GetSessionId());
	// Find Stream
	auto stream = std::static_pointer_cast<RtcStream>(GetStream(vhost_app_name, stream_name));
	if (!stream)
	{
		logte("To stop session failed. Cannot find stream (%s/%s)", vhost_app_name.CStr(), stream_name.CStr());
		return false;
	}

	auto session = std::static_pointer_cast<RtcSession>(stream->GetSession(offer_sdp->GetSessionId()));
	if (session == nullptr)
	{
		logte("To stop session failed. Cannot find session by peer sdp session id (%u)", offer_sdp->GetSessionId());
		return false;
	}

	DisconnectSessionInternal(session);
	_ice_port->RemoveSession(session);

	return true;
}

// bitrate info(from signalling)
uint32_t WebRtcPublisher::OnGetBitrate(const std::shared_ptr<WebSocketClient> &ws_client,
									   const info::VHostAppName &vhost_app_name, const ov::String &host_name, const ov::String &stream_name)
{
	auto stream = GetStream(vhost_app_name, stream_name);
	uint32_t bitrate = 0;

	if (!stream)
	{
		logte("Cannot find stream (%s/%s)", vhost_app_name.CStr(), stream_name.CStr());
		return 0;
	}

	auto tracks = stream->GetTracks();
	for (auto &track_iter : tracks)
	{
		MediaTrack *track = track_iter.second.get();

		if (track->GetCodecId() == cmn::MediaCodecId::Vp8 || track->GetCodecId() == cmn::MediaCodecId::Opus)
		{
			bitrate += track->GetBitrate();
		}
	}

	return bitrate;
}

bool WebRtcPublisher::OnIceCandidate(const std::shared_ptr<WebSocketClient> &ws_client,
									 const info::VHostAppName &vhost_app_name, const ov::String &host_name, const ov::String &stream_name,
									 const std::shared_ptr<RtcIceCandidate> &candidate,
									 const ov::String &username_fragment)
{
	return true;
}

/*
 * IcePort Implementation
 */

void WebRtcPublisher::OnStateChanged(IcePort &port, const std::shared_ptr<info::Session> &session_info,
									 IcePortConnectionState state)
{
	logtd("IcePort OnStateChanged : %d", state);

	auto session = std::static_pointer_cast<RtcSession>(session_info);
	auto application = session->GetApplication();
	auto stream = std::static_pointer_cast<RtcStream>(session->GetStream());

	// state를 보고 session을 처리한다.
	switch (state)
	{
		case IcePortConnectionState::New:
		case IcePortConnectionState::Checking:
		case IcePortConnectionState::Connected:
		case IcePortConnectionState::Completed:
			// 연결되었을때는 할일이 없다.
			break;
		case IcePortConnectionState::Failed:
		case IcePortConnectionState::Disconnected:
		case IcePortConnectionState::Closed:
		{
			logti("IcePort is disconnected. : (%s/%s/%u) reason(%d)", stream->GetApplicationName(), stream->GetName().CStr(), session->GetId(), state);
			DisconnectSessionInternal(session);
			break;
		}
		default:
			break;
	}
}

void WebRtcPublisher::OnDataReceived(IcePort &port, const std::shared_ptr<info::Session> &session_info, std::shared_ptr<const ov::Data> data)
{
	auto session = std::static_pointer_cast<pub::Session>(session_info);
	auto application = session->GetApplication();
	application->PushIncomingPacket(session_info, data);
}
