#include "rtp_rtcp.h"
#include "publishers/webrtc/rtc_application.h"
#include "publishers/webrtc/rtc_stream.h"
#include "rtcp_receiver.h"
#include <base/ovlibrary/byte_io.h>

#define OV_LOG_TAG "RtpRtcp"

RtpRtcp::RtpRtcp(uint32_t id, std::shared_ptr<pub::Session> session, const std::vector<uint32_t> &ssrc_list)
	        : SessionNode(id, pub::SessionNodeType::Rtp, session)
{
	// Cached to reduce the cost of dynamic_pointer_cast
	_rtc_session = std::dynamic_pointer_cast<RtcSession>(session);

    for(auto ssrc : ssrc_list)
    {
        auto rtcp_generator = std::make_shared<RtcpSRGenerator>(ssrc);
        _rtcp_sr_generators[ssrc] = rtcp_generator;
    }
}

RtpRtcp::~RtpRtcp()
{
    _rtcp_sr_generators.clear();
}

bool RtpRtcp::Stop()
{
	std::lock_guard<std::shared_mutex> lock(_session_lock);
	_rtc_session.reset();

	return SessionNode::Stop();
}

bool RtpRtcp::SendOutgoingData(const std::shared_ptr<RtpPacket> &rtp_packet)
{
	// Lower Node is SRTP
	auto node = GetLowerNode();
	if(!node)
	{
		return false;
	}

    if(_rtcp_sr_generators.find(rtp_packet->Ssrc()) != _rtcp_sr_generators.end())
    {
		auto rtcp_sr_generator = _rtcp_sr_generators[rtp_packet->Ssrc()];
		
		rtcp_sr_generator->AddRTPPacketAndGenerateRtcpSR(*rtp_packet);
		if(rtcp_sr_generator->IsAvailableRtcpSRPacket())
		{
			auto rtcp_sr_packet = rtcp_sr_generator->PopRtcpSRPacket();
			if(!node->SendData(pub::SessionNodeType::Rtcp, rtcp_sr_packet->GetData()))
			{
				logd("RTCP","Send RTCP failed : ssrc(%u)", rtp_packet->Ssrc());
			}
			else
			{
				logd("RTCP", "Send RTCP succeed : ssrc(%u) length(%d)", rtp_packet->Ssrc(), rtcp_sr_packet->GetData()->GetLength());
			}
		}
	}

	if(!node->SendData(pub::SessionNodeType::Rtp, rtp_packet->GetData()))
    {
		return false;
    }

	return true;
}

bool RtpRtcp::SendData(pub::SessionNodeType from_node, const std::shared_ptr<ov::Data> &data)
{
	return true;
}

// Implement SessionNode Interface
// decoded data from srtp
// no upper node( receive data process end)
bool RtpRtcp::OnDataReceived(pub::SessionNodeType from_node, const std::shared_ptr<const ov::Data> &data)
{
	// nothing to do before node start
	if(GetState() != SessionNode::NodeState::Started)
	{
		logtd("SessionNode has not started, so the received data has been canceled.");
		return false;
	}

	// Parse RTCP Packet
	RtcpReceiver receiver;
	if(receiver.ParseCompoundPacket(data) == false)
	{
		return false;
	}

	while(receiver.HasAvailableRtcpInfo())
	{
		auto info = receiver.PopRtcpInfo();
		
		std::shared_lock<std::shared_mutex> lock(_session_lock);
		if(_rtc_session != nullptr)
		{
			_rtc_session->OnRtcpReceived(info);
		}
	}

    return true;
}
