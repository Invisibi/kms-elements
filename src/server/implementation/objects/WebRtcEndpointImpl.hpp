#ifndef __WEB_RTC_ENDPOINT_IMPL_HPP__
#define __WEB_RTC_ENDPOINT_IMPL_HPP__

#include "BaseRtpEndpointImpl.hpp"
#include "WebRtcEndpoint.hpp"
#include <EventHandler.hpp>

namespace kurento
{

class MediaPipeline;
class WebRtcEndpointImpl;

void Serialize (std::shared_ptr<WebRtcEndpointImpl> &object,
                JsonSerializer &serializer);

class WebRtcEndpointImpl : public BaseRtpEndpointImpl,
  public virtual WebRtcEndpoint
{

public:

  WebRtcEndpointImpl (const boost::property_tree::ptree &conf,
                      std::shared_ptr<MediaPipeline> mediaPipeline);

  virtual ~WebRtcEndpointImpl () {};

  /* Next methods are automatically implemented by code generator */
  virtual bool connect (const std::string &eventType,
                        std::shared_ptr<EventHandler> handler);

  virtual void invoke (std::shared_ptr<MediaObjectImpl> obj,
                       const std::string &methodName, const Json::Value &params,
                       Json::Value &response);

  virtual void Serialize (JsonSerializer &serializer);

private:

  std::shared_ptr<std::string> getPemCertificate ();
  static std::mutex certificateMutex;

  class StaticConstructor
  {
  public:
    StaticConstructor();
  };

  static StaticConstructor staticConstructor;

};

} /* kurento */

#endif /*  __WEB_RTC_ENDPOINT_IMPL_HPP__ */
