import Foundation

enum DictationTransport: String, CaseIterable, Identifiable, Sendable {
    case usb
    case wifi

    var id: String { rawValue }
    var title: String {
        switch self {
        case .usb: return "USB"
        case .wifi: return "Wi-Fi"
        }
    }
}

enum DictationTransportPreference: String, CaseIterable, Identifiable, Sendable {
    case auto
    case usb
    case wifi

    var id: String { rawValue }
    var title: String {
        switch self {
        case .auto: return "Auto"
        case .usb: return "USB"
        case .wifi: return "Wi-Fi"
        }
    }

    func resolve(usbReady: Bool, wifiReady: Bool) -> DictationTransport? {
        switch self {
        case .auto: return usbReady ? .usb : (wifiReady ? .wifi : nil)
        case .usb: return usbReady ? .usb : nil
        case .wifi: return wifiReady ? .wifi : nil
        }
    }
}
