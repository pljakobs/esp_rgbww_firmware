#include <SmingCore.h>
#include <Network/Http/Websocket/WebsocketResource.h>

class wsStream : public Stream
{
public:
    size_t write(uint8_t c) override
    {
        msg = msg+String((char)c);
        if (c==0xa || c==0xc)
        {
            wsBroadcast( msg);
            msg="";
        }
        return 1;
    }

    size_t write(const uint8_t* buffer, size_t size) override
    {
        if(system_get_free_heap_size()>15000){ // do not overload memory, rather lose messages
            if (msg.length()>0)
            {
                msg=msg+"\n";
                wsBroadcast( msg);
                msg="";
            }
            msg=String((const char*)buffer, size);
            wsBroadcast(msg);
            return size;
        }
        return 0;
    }

    int available() override
    // no bytes available in a read only stream
    {
        return 0;
    }

    int read() override
    // read doesn't make sense in write only streams
    {
        return -1;
    }

    int peek() override
    // peek doesn't make sense in write only streams
    {
        return -1;
    }    

    void flush() override
    {

    }

    void wsBroadcast(String message)
    {
        JsonRpcMessage msg(F("log"));
	    JsonObject root = msg.getParams();
	    root[F("message")] = message;
	    String jsonStr = Json::serialize(msg.getRoot());
        app.webserver.wsBroadcast(jsonStr);
    }
private:
    String msg;
};