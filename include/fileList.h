#ifdef ARCH_ESP8266
#define WEBAPP_REDIRECT_FILE(XX) XX(webapp_redirect_html, "webapp-redirect.html")
#else
#define WEBAPP_REDIRECT_FILE(XX)
#endif

#define FILE_LIST(XX) \
        XX(captive_html, "captive.html") \
        XX(updating_html, "updating.html") \
        WEBAPP_REDIRECT_FILE(XX)
