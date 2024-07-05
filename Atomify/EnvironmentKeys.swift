import SwiftUI

struct SelectedSimulationKey: EnvironmentKey {
    static let defaultValue: Simulation? = nil
}

extension EnvironmentValues {
    var selectedSimulation: Simulation? {
        get { self[SelectedSimulationKey.self] }
        set { self[SelectedSimulationKey.self] = newValue }
    }
}
