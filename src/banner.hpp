#ifndef BANNER_HPP
#define BANNER_HPP

/*
 * =============================================================================
 * Banner Module - Optional Terminal Animation
 * =============================================================================
 * 
 * This module provides an optional animated banner display that can be shown
 * at program startup via the --banner command line flag.
 * 
 * DESIGN PHILOSOPHY:
 * - Completely isolated from core functionality
 * - No dependencies on common.hpp or other core modules
 * - Can be removed at any time by deleting banner.cpp and removing the
 *   --banner flag handling - no cleanup needed elsewhere
 * 
 * USAGE:
 *   high --banner
 * 
 * The banner displays an animated "HIGH" title with various visual effects
 * including particles, waves, and decorative elements.
 * =============================================================================
 */

namespace Banner {
    /*
     * Run the animated banner display.
     * 
     * This function takes control of the terminal, displays the animation,
     * and restores the terminal state when complete or interrupted.
     * 
     * @param duration_seconds - How long to display the banner (0 = until interrupt)
     */
    void run(float duration_seconds = 3.0f);
}

#endif
