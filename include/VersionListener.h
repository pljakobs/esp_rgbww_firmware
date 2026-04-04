/**
 * @author  Peter Jakobs http://github.com/pljakobs
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details at
 * https://www.gnu.org/copyleft/gpl.html
 *
 * @section DESCRIPTION
 *
 */
 
#include <JSON/Listener.h>
namespace JSON
{
    class VersionListener : public Listener
    {
    public:
        VersionListener()
        {
            version = 0;
            gotVersion = false;
        }

        ~VersionListener()
        {};

        bool startElement(const Element& element) override
        {
            debug_i("startElement: %s", element.key);
            if (F("version") == element.key)                {
                version = static_cast<uint8_t>(std::stoi(element.value));
                gotVersion = true;
                return false;
            }
            return true;
        }

        bool endElement(const Element& element) override
        {
            if (std::string(element.key) == "version")
            {
                return false;
            }
            return true;
        }

        bool hasVersion() const
        {
            return gotVersion;
        }

        uint8_t getVersion() const
        {
            return version;
        }

    private:
        uint8_t version;
        bool gotVersion;
    };
}//namespace JSON