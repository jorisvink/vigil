import Foundation
import UIKit
import Vigil

enum Avatar {
    static func jpeg(_ data: Data) throws -> Data {
        guard let picked = UIImage(data: data), let image = picked.cgImage else {
            throw ChatVault.Failure(description: "That is not an image.")
        }

        let side = min(image.width, image.height, 256)
        let size = CGSize(width: side, height: side)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1

        let scaled = UIGraphicsImageRenderer(size: size, format: format).image { _ in
            UIImage(cgImage: image).draw(in: CGRect(origin: .zero, size: size))
        }

        var quality: CGFloat = 0.85

        while quality > 0 {
            if let jpeg = scaled.jpegData(compressionQuality: quality), jpeg.count <= Int(VIGIL_CHAT_AVATAR_MAX) {
                return jpeg
            }

            quality -= 0.1
        }

        throw ChatVault.Failure(description: "That picture is too large.")
    }
}
