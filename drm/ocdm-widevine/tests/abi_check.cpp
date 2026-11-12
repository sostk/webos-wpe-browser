// Checks that cdm.h places every virtual in the vtable slot measured on the TV.
// Itanium C++ ABI (GCC on ARM): a pointer to a virtual member function holds 1 + the byte offset
// of its vtable entry; on ARM the "virtual" flag lives in the adjustment word instead, so the
// pointer field is the plain byte offset. Either way the slot is recoverable.
#include <widevine/cdm.h>

#include <cstdio>
#include <cstring>

using widevine::Cdm;

template<typename F>
static long slot_of(F member)
{
    struct Rep {
        long ptr;
        long adj;
    } rep;
    static_assert(sizeof(member) == sizeof(Rep), "unexpected member pointer size");
    std::memcpy(&rep, &member, sizeof(rep));
#if defined(__arm__) || defined(__aarch64__)
    if (!(rep.adj & 1))
        return -1; // not virtual
    return rep.ptr / static_cast<long>(sizeof(void*));
#else
    if (!(rep.ptr & 1))
        return -1;
    return (rep.ptr - 1) / static_cast<long>(sizeof(void*));
#endif
}

struct Expect {
    const char* name;
    long slot;
    long actual;
};

int main()
{
    // Slot numbers measured on the TV (docs/widevine-cdm-abi.md); 0 = onTimerExpired, 1-2 = destructor.
    Expect checks[] = {
        { "onTimerExpired", 0, slot_of(&Cdm::onTimerExpired) },
        { "setServiceCertificate", 3, slot_of(&Cdm::setServiceCertificate) },
        { "getServiceCertificateRequest", 4, slot_of(&Cdm::getServiceCertificateRequest) },
        { "getOemCryptoBuildInfo", 8, slot_of(&Cdm::getOemCryptoBuildInfo) },
        { "isProvisioned", 9, slot_of(&Cdm::isProvisioned) },
        { "getProvisioningRequest", 10, slot_of(&Cdm::getProvisioningRequest) },
        { "handleProvisioningResponse", 11, slot_of(&Cdm::handleProvisioningResponse) },
        { "createSession", 18, slot_of(&Cdm::createSession) },
        { "generateRequest", 19, slot_of(&Cdm::generateRequest) },
        { "IStorage::read", 0, slot_of(&Cdm::IStorage::read) },
        { "IStorage::list", 5, slot_of(&Cdm::IStorage::list) },
        { "IClock::now", 0, slot_of(&Cdm::IClock::now) },
        { "ITimer::setTimeout", 0, slot_of(&Cdm::ITimer::setTimeout) },
        { "ITimer::cancel", 1, slot_of(&Cdm::ITimer::cancel) },
    };
    int failures = 0;
    for (const Expect& e : checks) {
        bool ok = e.slot == e.actual;
        failures += !ok;
        std::printf("%-30s expected %2ld got %2ld %s\n", e.name, e.slot, e.actual, ok ? "ok" : "MISMATCH");
    }
    std::printf(failures ? "%d mismatch(es)\n" : "all slots match\n", failures);
    return failures ? 1 : 0;
}
