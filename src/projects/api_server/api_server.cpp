//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#include "api_server.h"

#include "api_private.h"
#include "controllers/root_controller.h"

#define API_VERSION "1"

namespace api
{
	bool Server::Start(const std::shared_ptr<cfg::Server> &server_config)
	{
		auto manager = HttpServerManager::GetInstance();

		// Port configurations
		const auto &api_bind_config = server_config->GetBind().GetManagers().GetApi();
		// API Server configurations
		const auto &managers = server_config->GetManagers();
		const auto &api_config = managers.GetApi();

		if (api_bind_config.IsParsed() == false)
		{
			logti("API Server is disabled");
			return true;
		}

		_access_token = api_config.GetAccessToken();

		auto http_interceptor = CreateInterceptor();

		bool http_server_result = true;
		bool is_parsed;
		auto &port = api_bind_config.GetPort(&is_parsed);
		ov::SocketAddress address;
		if (is_parsed)
		{
			address = ov::SocketAddress(server_config->GetIp(), port.GetPort());

			_http_server = manager->CreateHttpServer("APIServer", address);

			if (_http_server != nullptr)
			{
				_http_server->AddInterceptor(http_interceptor);
			}
			else
			{
				logte("Could not initialize HttpServer");
				http_server_result = false;
			}
		}

		bool https_server_result = true;
		auto &tls_port = api_bind_config.GetTlsPort(&is_parsed);
		ov::SocketAddress tls_address;
		if (is_parsed)
		{
			auto host_name_list = std::vector<ov::String>();

			for (auto &name : managers.GetHost().GetNameList())
			{
				host_name_list.push_back(name);
			}

			tls_address = ov::SocketAddress(server_config->GetIp(), tls_port.GetPort());
			auto certificate = info::Certificate::CreateCertificate("api_server", host_name_list, managers.GetHost().GetTls());

			if (certificate != nullptr)
			{
				_https_server = manager->CreateHttpsServer("APIServer", address, certificate);

				if (_https_server != nullptr)
				{
					_https_server->AddInterceptor(http_interceptor);
				}
				else
				{
					logte("Could not initialize HttpsServer");
					https_server_result = false;
				}
			}
		}

		if (http_server_result && https_server_result)
		{
			// Everything is OK
			logti("API Server is listening on %s", address.ToString().CStr());
			return true;
		}

		// Rollback
		manager->ReleaseServer(_http_server);
		manager->ReleaseServer(_https_server);

		return false;
	}

	std::shared_ptr<HttpRequestInterceptor> Server::CreateInterceptor()
	{
		auto http_interceptor = std::make_shared<HttpDefaultInterceptor>();

		// Request Handlers will be added to http_interceptor
		_root_controller = std::make_shared<RootController>();
		_root_controller->SetInterceptor(http_interceptor);
		_root_controller->PrepareHandlers();

		return http_interceptor;
	}

	bool Server::Stop()
	{
		auto manager = HttpServerManager::GetInstance();

		std::shared_ptr<HttpServer> http_server = std::move(_http_server);
		std::shared_ptr<HttpsServer> https_server = std::move(_https_server);

		bool http_result = (http_server != nullptr) ? manager->ReleaseServer(http_server) : false;
		bool https_result = (https_server != nullptr) ? manager->ReleaseServer(https_server) : false;

		return http_result && https_result;
	}

	void Server::RegisterHandlers()
	{
		// _handler_list.push_back();
	}
}  // namespace api
