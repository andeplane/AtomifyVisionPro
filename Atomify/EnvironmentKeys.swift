import SwiftUI

struct LammpsWrapperKey: EnvironmentKey {
    static let defaultValue: LammpsWrapper? = LammpsWrapper()
}

extension EnvironmentValues {
    var lammpsController: LammpsWrapper {
        get { self[LammpsWrapperKey.self]! }
        set { self[LammpsWrapperKey.self] = newValue }
    }
}
