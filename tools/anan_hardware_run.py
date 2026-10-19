#!/usr/bin/env python3
"""Guided receive-only bench journal for the operator's two ANAN-200D DIY rigs.

Uses the existing local automation bridge for reads only. Physical identity,
protocol, RF, audio and relay observations require the operator. No firmware,
profile, setting, tune, connection or transmit command is sent by this tool.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import os
import tempfile

from automation_probe import Bridge, discover_socket

CASES = tuple((device, protocol) for protocol in (1, 2)
              for device in ("Anvelina", "Red Pitaya"))
CHECKS = (
    ("discovery", "Discovery und Verbindung zum richtigen realen Geraet funktionieren"),
    ("direct_ip", "Connect by IP funktioniert mit explizitem P1/P2 und gespeichertem Override"),
    ("receive", "Bekanntes Empfangssignal, Abstimmung, Audio, Panadapter und Waterfall stimmen"),
    ("rates", "48/96/192 ksps (P1) bzw. 48/96/192/384/768/1536 ksps (P2) funktionieren"),
    ("adc1", "ADC1 ist nach Neuverbindung aktiv und empfaengt am tatsaechlichen ADC1-Eingang"),
    ("inputs", "ADC0-Eingaenge und ANT1/2/3 schalten korrekt; ADC1 bleibt davon unabhaengig"),
    ("alex", "ANAN-200D HPF/LPF-Relais folgen Bandwechseln, Bypass und Disable 6m LNA on RX"),
    ("alex2_hidden", "Alex-2 bleibt mit ANAN-200D-Mapping ausgeblendet"),
    ("recall", "Mapping, ANT-Bank und Filtereinstellungen bleiben nach Neuverbindung erhalten"),
)


def save(path, report):
    """Replace the journal atomically after every completed observation."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def request(bridge, obj):
    response = bridge.request(obj, timeout_seconds=15)
    if not isinstance(response, dict) or response.get("ok") is False:
        raise RuntimeError("Bridge-Anfrage fehlgeschlagen: " + str(obj.get("model", obj["cmd"])))
    return response


def radio(bridge):
    state = request(bridge, {"cmd": "get", "model": "radio"}).get("radio", {})
    if not state.get("connected") or state.get("model") != "ANAN-200D":
        raise RuntimeError("Ein verbundenes Geraet mit effektivem Modell ANAN-200D wird benoetigt.")
    if state.get("transmitting"):
        raise RuntimeError("Die Pruefrunde ist nur fuer Empfang vorgesehen.")
    if not state.get("serial"):
        raise RuntimeError("Die verbundene Geraeteidentitaet fehlt.")
    return state


def observed(prompt):
    while True:
        choice = input(prompt + " [j=erfolgreich/n=Fehler/s=nicht geprueft]: ").strip().lower()
        if choice in {"j", "n", "s"}:
            return {"j": "operator_pass", "n": "operator_fail", "s": "not_tested"}[choice]


def outcome(checks):
    if "operator_fail" in checks.values():
        return "operator_reported_failure"
    return "operator_checks_complete" if len(checks) == len(CHECKS) and all(
        value == "operator_pass" for value in checks.values()) else "incomplete"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", action="store_true", help="Show the four cases without contacting Aether")
    parser.add_argument("--socket", help="Explicit local automation endpoint")
    parser.add_argument("--output", type=Path, help="New JSON journal; existing files are refused")
    args = parser.parse_args()
    if args.plan:
        for device, protocol in CASES:
            print(f"{device}: P{protocol}, Override ANAN-200D, ANAN-200D LPF/PA")
        return 0
    path = args.output or Path("anan-hardware-results") / (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".json")
    if path.exists():
        parser.error("Ausgabedatei existiert bereits; bitte einen neuen Namen verwenden.")
    endpoint = args.socket or discover_socket()
    if not endpoint:
        parser.error("Aethers lokale Automation Bridge ist nicht verfuegbar. Siehe ANAN-Benutzerdokumentation.")
    report = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
              "scope": "two physical DIY devices, each P1/P2, effective ANAN-200D",
              "evidence": "client snapshots plus operator observations; not independent RF certification",
              "physicalIdentityAndProtocol": "operator-confirmed, not derived from display model",
              "notQualified": "native Anvelina/Red Pitaya profiles and all other models",
              "status": "incomplete", "cases": []}
    bridge = None
    try:
        bridge = Bridge(endpoint)
        report["build"] = request(bridge, {"cmd": "ping"})
        save(path, report)
        print("Vier Empfangsfaelle. Geraete und Protokoll werden von dir verbunden/umgeschaltet.")
        print("Kein radiocert all/tx/meters verwenden. Token und Aether-Einstellungen bleiben unveraendert.")
        for device, protocol in CASES:
            case = {"device": device, "protocol": protocol, "effectiveModel": "ANAN-200D",
                    "filterHardware": "ANAN-200D LPF/PA", "checks": {}, "status": "incomplete"}
            report["cases"].append(case)
            save(path, report)
            print(f"\n=== {device}, P{protocol}, Override ANAN-200D ===")
            ready = input("Reales Geraet mit diesem Protokoll verbinden; P1/P2 Gateware in Radio Info pruefen. Enter=bereit, s=auslassen: ").strip().lower()
            if ready == "s":
                case["status"] = "not_tested"
                save(path, report)
                continue
            case["protocolConfirmedByOperator"] = True
            try:
                case["before"] = radio(bridge)
            except RuntimeError as error:
                case["status"] = "not_ready"
                case["error"] = str(error)
                print(error)
                save(path, report)
                continue
            save(path, report)
            for key, label in CHECKS:
                case["checks"][key] = observed(label)
                save(path, report)
            case["notes"] = input("Firmwareversion und Besonderheiten/Fehler: ").strip()
            try:
                case["after"] = radio(bridge)
                if case["after"]["serial"] != case["before"]["serial"]:
                    raise RuntimeError("Die Geraeteidentitaet hat sich innerhalb eines Testfalls geaendert.")
            except RuntimeError as error:
                case["status"] = "invalid_final_identity"
                case["error"] = str(error)
                print(error)
                save(path, report)
                continue
            case["slices"] = request(bridge, {"cmd": "get", "model": "slices"})
            case["pans"] = request(bridge, {"cmd": "get", "model": "pans"})
            case["status"] = outcome(case["checks"])
            save(path, report)
        report["status"] = "operator_checks_complete" if all(
            case["status"] == "operator_checks_complete" for case in report["cases"]) else "incomplete_or_failed"
        save(path, report)
        print(f"\nProtokoll: {path.resolve()}\nStatus: {report['status']}")
        return 0 if report["status"] == "operator_checks_complete" else 1
    except (KeyboardInterrupt, EOFError, OSError, ValueError, RuntimeError) as error:
        report["status"] = "interrupted"
        report["errorType"] = type(error).__name__
        save(path, report)
        print(f"Abgebrochen: {error}\nTeilprotokoll: {path.resolve()}")
        return 2
    finally:
        if bridge:
            bridge.close()


if __name__ == "__main__":
    raise SystemExit(main())
