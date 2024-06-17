//
//  AtomifyApp.swift
//  Atomify
//
//  Created by Anders Hafreager on 15/06/2024.
//

import SwiftUI
import Foundation
// import LAMMPS
@main
struct AtomifyApp: App {
    @State private var atomStyle: ImmersionStyle = .full
    init() {
        let filepath = Bundle.main.path(forResource: "vapor", ofType: "data", inDirectory: "simulations/water/vapor")
        print("Path: \(filepath)")
        
        let filepath2 = Bundle.main.path(forResource: "vapor", ofType: "data", inDirectory: "Atomify")
        print("Path: \(filepath2)")
        
        let path = "\(Bundle.main.resourcePath!)/Atomify/simulations/water/vapor/vapor.data"
        print("Second path: \(path)")
        do {
            let text =
                try String(contentsOfFile: path, encoding: String.Encoding.utf8)
        } catch {
            print("No file actually")
        }
        
        let resourceURL = Bundle.main.resourceURL!

        do {
            let resourceContents = try FileManager.default.contentsOfDirectory(at: resourceURL, includingPropertiesForKeys: nil, options: .skipsHiddenFiles)
            for url in resourceContents {
                print(url.path)
            }
        } catch {
            print("Error: \(error)")
        }
    }
    var body: some Scene {
        WindowGroup {
            ContentView()
        }

        ImmersiveSpace(id: "ImmersiveSpace") {
            ImmersiveView()
        }.immersionStyle(selection: $atomStyle, in: .full)
    }
}
