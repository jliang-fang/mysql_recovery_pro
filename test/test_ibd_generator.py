#!/usr/bin/env python3
"""Tests for ibd_generator.py"""

import sys
import os

# Add the tools directory to the path so we can import ibd_generator
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))

import ibd_generator


def test_get_factorial():
    """Test get_factorial function"""
    # Test basic cases
    assert ibd_generator.get_factorial(0) == 1, "factorial(0) should be 1"
    assert ibd_generator.get_factorial(1) == 1, "factorial(1) should be 1"
    assert ibd_generator.get_factorial(5) == 120, "factorial(5) should be 120"
    assert ibd_generator.get_factorial(10) == 3628800, "factorial(10) should be 3628800"

    # Test error case
    try:
        ibd_generator.get_factorial(-1)
        assert False, "Should have raised ValueError for negative input"
    except ValueError:
        pass

    print("All get_factorial tests passed!")


if __name__ == "__main__":
    test_get_factorial()
