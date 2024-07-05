import Foundation
import SwiftUI

class Simulation: Identifiable, ObservableObject, Equatable {
    static func == (lhs: Simulation, rhs: Simulation) -> Bool {
        lhs.id == rhs.id
    }
    
    var id: String
    var title: String
    var description: String
    var analysisDescription: String?
    var imageUrl: String
    var inputScript: String
    var keywords: [String]
    var files: [File]
    @Published var imageFile: UIImage?
    
    struct File {
        var fileName: String
        var url: String
    }
    
    init(id: String, title: String, description: String, analysisDescription: String?, imageUrl: String, inputScript: String, keywords: [String], files: [File], imageFile: UIImage?) {
        self.id = id
        self.title = title
        self.description = description
        self.analysisDescription = analysisDescription
        self.imageUrl = imageUrl
        self.inputScript = inputScript
        self.keywords = keywords
        self.files = files
        self.imageFile = imageFile
        self.loadImage()
    }
    
    convenience init?(json: [String: Any]) {
        guard let id = json["id"] as? String,
              let title = json["title"] as? String,
              let description = json["description"] as? String,
              let imageUrl = json["imageUrl"] as? String,
              let inputScript = json["inputScript"] as? String,
              let keywords = json["keywords"] as? [String],
              let filesArray = json["files"] as? [[String: Any]] else {
            return nil
        }
        
        var files: [File] = []
        for fileJson in filesArray {
            if let fileName = fileJson["fileName"] as? String,
               let url = fileJson["url"] as? String {
                files.append(File(fileName: fileName, url: url))
            }
        }
        
        self.init(id: id, title: title, description: description, analysisDescription: json["analysisDescription"] as? String, imageUrl: imageUrl, inputScript: inputScript, keywords: keywords, files: files, imageFile: nil)
    }
    
    func loadImage() {
        DispatchQueue.global().async {
            if let imagePath = Bundle.main.path(forResource: self.imageUrl, ofType: nil),
               let image = UIImage(contentsOfFile: imagePath) {
                DispatchQueue.main.async {
                    self.imageFile = image
                }
            } else {
                DispatchQueue.main.async {
                    self.imageFile = UIImage(named: "placeholder")
                }
            }
        }
    }
    
    static func loadSimulations() throws -> [Simulation]? {
        let fileManager = FileManager.default
        if let resourcePath = Bundle.main.resourcePath {
            let simulationsDirectoryPath = resourcePath.appending("/simulations")
            
            do {
                let files = try fileManager.contentsOfDirectory(atPath: simulationsDirectoryPath)
                print("Files in directory \(simulationsDirectoryPath): \(files)")
            } catch {
                print("Error reading contents of directory \(simulationsDirectoryPath): \(error)")
                throw error
            }
        } else {
            print("Resource path not found.")
            throw NSError(domain: "Simulation", code: 1, userInfo: [NSLocalizedDescriptionKey: "Resource path not found."])
        }

        guard let path = Bundle.main.path(forResource: "simulations", ofType: "json", inDirectory: "simulations") else {
            print("simulations.json file path not found.")
            throw NSError(domain: "Simulation", code: 2, userInfo: [NSLocalizedDescriptionKey: "simulations.json file path not found."])
        }
        
        print("simulations.json file path: \(path)")
        
        let data: Data
        do {
            data = try Data(contentsOf: URL(fileURLWithPath: path))
        } catch {
            print("Failed to load data from simulations.json file.")
            throw error
        }
        
        print("simulations.json data loaded successfully.")
        
        let json: [String: Any]
        do {
            json = try JSONSerialization.jsonObject(with: data, options: []) as! [String: Any]
        } catch {
            print("Failed to parse JSON data.")
            throw error
        }
        
        print("simulations.json parsed successfully.")
        
        guard let jsonArray = json["examples"] as? [[String: Any]] else {
            print("Failed to parse examples array from JSON data.")
            throw NSError(domain: "Simulation", code: 3, userInfo: [NSLocalizedDescriptionKey: "Failed to parse examples array from JSON data."])
        }
        
        var simulations: [Simulation] = []
        for json in jsonArray {
            if let simulation = Simulation(json: json) {
                simulations.append(simulation)
            } else {
                print("Failed to parse simulation from JSON: \(json)")
            }
        }
        return simulations
    }
}
