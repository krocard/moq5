import MoQTransportModel

/// A MoQ transport draft version this SDK can negotiate.
///
/// The published `MoQVersion` spelling is preserved, but it now names the
/// SHARED nominal type `MoQTransportModel.MoQTransportVersion` rather than
/// a service-local enum. `MoQMedia`'s media-facing alias denotes that same
/// type, so a negotiated version crosses the service/media boundary with no
/// conversion and no second enum to drift from this one.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
public typealias MoQVersion = MoQTransportVersion

/// What versions an endpoint offers during negotiation.
///
/// Mirrors the C `moq_version_offer_t` policies: `.automatic` offers every
/// version this build supports (never "pick newest and hope"), `.list` offers
/// exactly the given set in preference order, `.exactly` pins one version and
/// makes a mismatch a terminal connect failure.
@available(macOS 13.0, iOS 16.0, tvOS 16.0, watchOS 9.0, *)
public enum VersionOffer: Sendable, Hashable {
    case automatic
    case list([MoQVersion])
    case exactly(MoQVersion)
}
