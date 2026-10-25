#pragma once

#include <QAbstractSocket>
#include <QHostAddress>
#include <QList>
#include <QString>

namespace AetherSDR {

struct NetworkPathCandidate {
    QString      interfaceId;
    QString      interfaceName;
    QHostAddress address;

    bool isValid() const;
    QString label() const;
};

class NetworkPathResolver {
public:
    using Family = QAbstractSocket::NetworkLayerProtocol;

    static QList<NetworkPathCandidate> enumerateIpv4Candidates();
    //DH1KLM Source addresses of one family; IPv6 leaves out link-local. A saved
    //DH1KLM path matches by interface first, so a rotating IPv6 address still resolves.
    static QList<NetworkPathCandidate> enumerateCandidates(Family family);
    //DH1KLM A saved path resolves to its interface's address in `family`, so a path
    //DH1KLM chosen for an IPv4 radio still applies when the radio is reached over IPv6.
    static NetworkPathCandidate resolveExplicitSelection(const QString& interfaceId,
                                                         const QString& interfaceName,
                                                         const QHostAddress& lastSuccessfulAddress,
                                                         Family family = QAbstractSocket::IPv4Protocol,
                                                         const QHostAddress& target = {});
    //DH1KLM The candidate a saved selection names, or -1: the exact saved address
    //DH1KLM on the saved interface, else the interface by id, then by name, else the
    //DH1KLM exact address on any interface. An IPv6 interface usually carries both a
    //DH1KLM GUA and a ULA, so the exact address must win over "first on the interface";
    //DH1KLM without it, the interface's address sharing the longest prefix with
    //DH1KLM `target` wins (RFC 6724 rule 8), so a ULA radio gets the ULA source.
    static qsizetype matchSelection(const QList<NetworkPathCandidate>& candidates,
                                    const QString& interfaceId,
                                    const QString& interfaceName,
                                    const QHostAddress& lastSuccessfulAddress,
                                    const QHostAddress& target = {});
    static NetworkPathCandidate autoCandidate();
    static bool isUsableIpv4(const QHostAddress& address);
    static bool isUsableSourceAddress(const QHostAddress& address);
    //DH1KLM The family a connect target needs: IPv6 for an IPv6 literal, else IPv4
    //DH1KLM (an IPv4 literal, or a name, which has always been probed over IPv4).
    static Family targetFamily(const QString& host);
};

} // namespace AetherSDR
