/**
 * @file host_identity.cpp
 * @brief Host-emulator-only override for the chip identity.
 *
 * The Sming Host emulator returns a fixed value (0xC001BEAF) from
 * system_get_chip_id(). Every device identity in the firmware derives from
 * that value — the mDNS hostname (lightinator-<id>), the mDNS TXT "id" that
 * peers use to deduplicate the swarm, the MQTT client id, the default device
 * name and the "self" detection in Controllers. Running several Host instances
 * on one bridged network would therefore collapse them into a single logical
 * node.
 *
 * To let a swarm of Host instances gossip realistically, this translation unit
 * provides a linker --wrap override (Host build only, wired up in component.mk)
 * that returns the value of the LI_CHIP_ID environment variable when set,
 * falling back to the real Host value otherwise. LI_CHIP_ID accepts decimal or
 * 0x-prefixed hex, e.g. LI_CHIP_ID=0x1001 ./app ...
 */

#ifdef ARCH_HOST

#include <cstdint>
#include <cstdlib>

extern "C" uint32_t __real_system_get_chip_id(void);

extern "C" uint32_t __wrap_system_get_chip_id(void)
{
	const char* env = getenv("LI_CHIP_ID");
	if(env != nullptr && env[0] != '\0') {
		// strtoul with base 0 accepts decimal and 0x-prefixed hex.
		return static_cast<uint32_t>(strtoul(env, nullptr, 0));
	}
	return __real_system_get_chip_id();
}

#endif // ARCH_HOST
