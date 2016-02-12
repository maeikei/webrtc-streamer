/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** main.cpp
** 
** -------------------------------------------------------------------------*/

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <map>
#include <list>

#include "PeerConnectionManager.h"

#include "webrtc/base/ssladapter.h"
#include "webrtc/base/thread.h"
#include "webrtc/p2p/base/stunserver.h"
#include "webrtc/base/httpserver.h"
#include "webrtc/base/pathutils.h"

/* ---------------------------------------------------------------------------
**  http callback
** -------------------------------------------------------------------------*/
class HttpServerRequestHandler : public sigslot::has_slots<> 
{
	public:
		HttpServerRequestHandler(rtc::HttpServer* server, PeerConnectionManager* webRtcServer) : m_server(server), m_webRtcServer(webRtcServer)
		{
			m_server->SignalHttpRequest.connect(this, &HttpServerRequestHandler::OnRequest);
		}

		void OnRequest(rtc::HttpServer*, rtc::HttpServerTransaction* t) 
		{
			std::string host;
			std::string path;
			t-> request.getRelativeUri(&host, &path);
			std::cout << "===>" <<  path << std::endl;
			
			rtc::HttpAttributeList attributes;
			rtc::HttpParseAttributes(t-> request.path.c_str(), t-> request.path.size(), attributes);

			rtc::scoped_ptr<rtc::StreamInterface>& stream(t-> request.document);
			size_t size = 0;
			stream->GetSize(&size);
			stream->Rewind();
			char buffer[size];
			size_t readsize = 0;
			rtc::StreamResult res = stream->ReadAll(&buffer, size, &readsize, NULL);
			std::cout << "res:" << res << std::endl;
			std::string body(buffer, readsize);			
			std::cout << "readsize:" << readsize << std::endl;
			std::cout << "body:" << body << std::endl;
			
			std::string answer;			
			if (path == "/getDeviceList")
			{
				std::string answer(Json::StyledWriter().write(m_webRtcServer->getDeviceList()));
				
				rtc::MemoryStream* mem = new rtc::MemoryStream(answer.c_str(), answer.size());			
				t->response.set_success("text/plain", mem);			
			}
			else if (path == "/offer")
			{
				std::string url;
				t-> request.hasHeader("url", &url);
				url = rtc::s_url_decode(url);
				std::string peerid;					
				std::string answer(m_webRtcServer->getOffer(peerid,url));
				std::cout << peerid << ":" << answer << std::endl;
				
				rtc::MemoryStream* mem = new rtc::MemoryStream(answer.c_str(), answer.size());			
				t->response.addHeader("peerid",peerid);	
				t->response.set_success("text/plain", mem);			
			}
			else if (path == "/answer")
			{
				std::string peerid;	
				t-> request.hasHeader("peerid", &peerid);				
				m_webRtcServer->setAnswer(peerid, body);
				
				t->response.set_success();			
			}
			else if (path == "/getIceCandidate")
			{
				std::string peerid;	
				t-> request.hasHeader("peerid", &peerid);				
				
				std::string answer(Json::StyledWriter().write(m_webRtcServer->getIceCandidateList(peerid)));					
				rtc::MemoryStream* mem = new rtc::MemoryStream(answer.c_str(), answer.size());			
				t->response.set_success("text/plain", mem);			
			}
			else if (path == "/addIceCandidate")
			{
				std::string peerid;	
				t-> request.hasHeader("peerid", &peerid);				
				m_webRtcServer->addIceCandidate(peerid, body);	
				
				t->response.set_success();			
			}
			else
			{
				rtc::Pathname pathname("index.html");
				rtc::FileStream* fs(rtc::Filesystem::OpenFile(pathname, "rb"));
				t->response.set_success("text/html", fs);			
			}
			t->response.setHeader(rtc::HH_CONNECTION,"Close");
			m_server->Respond(t);
		}
		
	protected:
		rtc::HttpServer*       m_server;
		PeerConnectionManager* m_webRtcServer;
};

/* ---------------------------------------------------------------------------
**  main
** -------------------------------------------------------------------------*/
int main(int argc, char* argv[]) {
	const char* port     = "0.0.0.0:8000";
	const char* stunurl  = "stun.l.google.com:19302";
	int logLevel = rtc::LERROR; 
	
	int c = 0;     
	while ((c = getopt (argc, argv, "hS:H:v::")) != -1)
	{
		switch (c)
		{
			case 'v': logLevel--; if (optarg) logLevel-=strlen(optarg); break;
			case 'H': port = optarg; break;
			case 'S': stunurl = optarg; break;
			case 'h':
			default:
				std::cout << argv[0] << " [-P http port] [-S stun address] -[v[v]]"                             << std::endl;
				std::cout << "\t -v[v[v]]         : verbosity"                                                  << std::endl;
				std::cout << "\t -H [hostname:]port : HTTP server binding (default "   << port    << ")"        << std::endl;
				std::cout << "\t -S stun_address    : STUN listenning address (default " << stunurl << ")"        << std::endl;
				exit(0);
		}
	}
	
	rtc::LogMessage::LogToDebug((rtc::LoggingSeverity)logLevel);
	rtc::LogMessage::LogTimestamps();
	rtc::LogMessage::LogThreads();
	std::cout << "Logger level:" <<  rtc::LogMessage::GetMinLogSeverity() << std::endl; 
	
	rtc::Thread* thread = rtc::Thread::Current();
	rtc::InitializeSSL();
	
	// webrtc server
	PeerConnectionManager webRtcServer(stunurl);
	if (!webRtcServer.InitializePeerConnection())
	{
		std::cout << "Cannot Initialize WebRTC server" << std::endl; 
	}
	else
	{
		// http server
		rtc::HttpListenServer httpServer;
		rtc::SocketAddress http_addr;
		http_addr.FromString(port);
		int ret = httpServer.Listen(http_addr);
		if (ret != 0)
		{
			std::cout << "Cannot Initialize start HTTP server " << strerror(ret) << std::endl; 
		}
		else
		{
			// connect httpserver to a request handler
			HttpServerRequestHandler http(&httpServer, &webRtcServer);
			std::cout << "HTTP Listening at " << http_addr.ToString() << std::endl;
			
			// STUN server
			rtc::SocketAddress server_addr;
			server_addr.FromString(stunurl);
			rtc::scoped_ptr<cricket::StunServer> stunserver;
			rtc::AsyncUDPSocket* server_socket = rtc::AsyncUDPSocket::Create(thread->socketserver(), server_addr);
			if (server_socket) 
			{
				stunserver.reset(new cricket::StunServer(server_socket));
				std::cout << "STUN Listening at " << server_addr.ToString() << std::endl;
			}
			else
			{
				std::cout << "STUN bad at " << server_addr.ToString() << std::endl;
			}

			// mainloop
			while(thread->ProcessMessages(10));
		}
	}
	
	rtc::CleanupSSL();
	return 0;
}

