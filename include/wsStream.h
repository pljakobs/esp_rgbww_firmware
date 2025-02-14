#include <SmingCore.h>
#include <Network/Http/Websocket/WebsocketResource.h>

class wsStream : public Stream
{
public:
    size_t write(uint8_t c) override
    {
        msg = msg+String((char)c);
        if (c==0x0a || c==0x0c || msg.length()>=32)
        {
            wsLog( msg);
            msg="";
        }
        return 1;
    }

    size_t ICACHE_FLASH_ATTR write(const uint8_t* buffer, size_t size) override
    {
        if(system_get_free_heap_size()>18000){ // do not overload memory, rather lose messages
            if (msg.length()>0)
            {
                //msg=msg+"\n";
                wsLog( msg);
                msg="";
            }
            msg=String((const char*)buffer, size);
            wsLog(msg);
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
        if (msg.length() > 0) {
            wsLog(msg);
            msg = "";
        }
    }

    void ICACHE_FLASH_ATTR wsLog(String message){
		app.wsBroadcast(F("log"),message);
    }
private:
    String msg;
};