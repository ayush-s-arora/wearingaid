#ifndef KEY_PROFILES_H
#define KEY_PROFILES_H

#include <array>

namespace AudioEngine {
    
    enum class ProfileType {
        TEMPERLEY,      // Classical / Jazz
        SHAATH,         // Rock / Pop / Contemporary Bands
        EDMA,           // Electronic / House / Techno
        WEI_CHAI,       // Folk / Acoustic / Traditional
        TONIC_TRIAD     // Pure structural baseline
    };

    namespace Profiles {
        
        // Classical, Traditional (Temperley)
        constexpr double TEMPERLEY_MAJ_RAW[12] = { 5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0 };
        constexpr double TEMPERLEY_MIN_RAW[12] = { 5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0 };

        // Rock, Pop, Live Contemporary, Jazz (Shaath)
        constexpr double SHAATH_MAJ_RAW[12] = { 6.6, 2.0, 3.5, 2.3, 4.6, 4.0, 2.5, 5.2, 2.4, 3.7, 2.3, 3.4 };
        constexpr double SHAATH_MIN_RAW[12] = { 6.5, 2.7, 3.5, 5.4, 2.6, 3.5, 2.5, 5.2, 4.0, 2.7, 4.3, 3.2 };

        // EDM, House, Techno (EDMA)
        constexpr double EDMA_MAJ_RAW[12] = { 1.00, 0.29, 0.50, 0.40, 0.60, 0.56, 0.32, 0.80, 0.31, 0.45, 0.42, 0.39 };
        constexpr double EDMA_MIN_RAW[12] = { 1.00, 0.31, 0.44, 0.58, 0.33, 0.49, 0.29, 0.78, 0.43, 0.29, 0.53, 0.32 };

        // Folk, Acoustic, Traditional, Solo Instrumental (Wei Chai)
        constexpr double WEI_CHAI_MAJ_RAW[12] = { 81302, 320, 65719, 1916, 77469, 40928, 2223, 83997, 1218, 39853, 1579, 28908 };
        constexpr double WEI_CHAI_MIN_RAW[12] = { 39853, 1579, 28908, 81302, 320, 65719, 1916, 77469, 40928, 2223, 83997, 1218 };
        
        // Ambient, Drone, Minimalist, Strict Electronic Pop
        constexpr double TONIC_TRIAD_MAJ_RAW[12] = { 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 };
        constexpr double TONIC_TRIAD_MIN_RAW[12] = { 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 };

        namespace CompileTime {
            // L1 norm
            constexpr std::array<double, 12> normalize(const double (&raw_arr)[12]) {
                std::array<double, 12> normalized_arr{};
                double sum = 0.0;
                
                for (int i = 0; i < 12; ++i) {
                    sum += raw_arr[i];
                }
                
                // Divide by zero guard
                if (sum == 0.0) sum = 1.0; 

                // Normalize bins for probability distribution. Normalized values required for matrix operations.
                for (int i = 0; i < 12; ++i) {
                    normalized_arr[i] = raw_arr[i] / sum;
                }
                
                return normalized_arr;
            }
        }
        
        constexpr auto TEMPERLEY_MAJ   = CompileTime::normalize(TEMPERLEY_MAJ_RAW);
        constexpr auto TEMPERLEY_MIN   = CompileTime::normalize(TEMPERLEY_MIN_RAW);
        
        constexpr auto SHAATH_MAJ      = CompileTime::normalize(SHAATH_MAJ_RAW);
        constexpr auto SHAATH_MIN      = CompileTime::normalize(SHAATH_MIN_RAW);
        
        constexpr auto EDMA_MAJ        = CompileTime::normalize(EDMA_MAJ_RAW);
        constexpr auto EDMA_MIN        = CompileTime::normalize(EDMA_MIN_RAW);
        
        constexpr auto WEI_CHAI_MAJ    = CompileTime::normalize(WEI_CHAI_MAJ_RAW);
        constexpr auto WEI_CHAI_MIN    = CompileTime::normalize(WEI_CHAI_MIN_RAW);
        
        constexpr auto TONIC_TRIAD_MAJ = CompileTime::normalize(TONIC_TRIAD_MAJ_RAW);
        constexpr auto TONIC_TRIAD_MIN = CompileTime::normalize(TONIC_TRIAD_MIN_RAW);

    }
}

#endif // KEY_PROFILES_H