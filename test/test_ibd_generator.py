#!/usr/bin/env python3
"""Tests for ibd_generator.py"""

import sys
import os

# Add the tools directory to the path so we can import ibd_generator
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))

import ibd_generator


def test_get_factorial():
    """Test factorial implementation consistency"""
    # This is a placeholder for testing factorial logic if needed in Python
    pass


if __name__ == "__main__":
    test_get_factorial()
