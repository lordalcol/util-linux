#!/usr/bin/env python3
"""
Comprehensive test script for the minas program.
Tests each filter individually, both stdout and stderr, and negative cases.
"""

import subprocess
import time
import sys
import os

def run_minas_test(test_name, command, output_file):
    """Run a minas test and return the results."""
    print(f"\n{test_name}")
    print("-" * len(test_name))
    
    try:
        result = subprocess.run([
            "./minas", 
            "-c", command,
            output_file
        ], capture_output=True, text=True, timeout=30)
        
        print("Minas stdout:", result.stdout.strip())
        if result.stderr:
            print("Minas stderr:", result.stderr.strip())
        
        if os.path.exists(output_file):
            print(f"\nOutput file content ({output_file}):")
            print("-" * 40)
            with open(output_file, "r") as f:
                content = f.read()
                print(content)
            return content
        else:
            print("No output file was created!")
            return ""
            
    except subprocess.TimeoutExpired:
        print("Test timed out!")
        return ""
    except Exception as e:
        print(f"Error running test: {e}")
        return ""

def test_individual_filters(results=None):
    """Test each filter individually for both stdout and stderr."""
    
    if results is None:
        results = {
            'error:': {'stdout': False, 'stderr': False, 'negative': False},
            'ERROR:': {'stdout': False, 'stderr': False, 'negative': False},
            'Linking ': {'stdout': False, 'stderr': False, 'negative': False},
            'warning': {'stdout': False, 'stderr': False, 'negative': False},
            'Running ': {'stdout': False, 'stderr': False, 'negative': False},
            'Run ': {'stdout': False, 'stderr': False, 'negative': False},
        }
    
    print("Testing Individual Filters (stdout and stderr)")
    print("=" * 50)
    
    # Test each filter keyword for both stdout and stderr
    filters = [
        ("error: (lowercase)", "echo 'error: this should be logged'", 'error:'),
        ("ERROR: (uppercase)", "echo 'ERROR: this should be logged'", 'ERROR:'),
        ("Linking (with space)", "echo 'Linking libraries...'", 'Linking '),
        ("warning (lowercase)", "echo 'warning: this should be logged'", 'warning'),
        ("Running (with space)", "echo 'Running tests...'", 'Running '),
        ("Run (with space)", "echo 'Run ./configure first'", 'Run '),
    ]
    
    for filter_name, command, filter_key in filters:
        # Test stdout
        output_file = f"filter_test_{filter_name.replace(' ', '_').replace('(', '').replace(')', '').replace(':', '')}_stdout.txt"
        content = run_minas_test(f"{filter_name} (stdout)", command, output_file)
        
        # Check if the filter worked (content should contain the expected line with timestamp)
        if filter_key in content and '2025-' in content:
            results[filter_key]['stdout'] = True
        
        # Test stderr (same command, just redirect to stderr)
        stderr_command = command + " >&2"
        output_file = f"filter_test_{filter_name.replace(' ', '_').replace('(', '').replace(')', '').replace(':', '')}_stderr.txt"
        content = run_minas_test(f"{filter_name} (stderr)", stderr_command, output_file)
        
        # Check if the filter worked for stderr
        if filter_key in content and '2025-' in content:
            results[filter_key]['stderr'] = True
        
        time.sleep(1)  # Small delay between tests
    
    return results

def test_negative_cases(results=None):
    """Test lines that should NOT be logged."""
    
    if results is None:
        results = {
            'error:': {'stdout': False, 'stderr': False, 'negative': False},
            'ERROR:': {'stdout': False, 'stderr': False, 'negative': False},
            'Linking ': {'stdout': False, 'stderr': False, 'negative': False},
            'warning': {'stdout': False, 'stderr': False, 'negative': False},
            'Running ': {'stdout': False, 'stderr': False, 'negative': False},
            'Run ': {'stdout': False, 'stderr': False, 'negative': False},
        }
    
    print("\n\nTesting Negative Cases (Should NOT be logged)")
    print("=" * 50)
    
    # Lines that should NOT be logged - test specific filter issues
    negative_tests = [
        ("Error without colon", "echo 'This error message should not be logged'", 'error:'),
        ("Warning without colon", "echo 'This warning message should not be logged'", 'warning'),
        ("Partial match", "echo 'This contains error but no colon'", 'error:'),
        ("Case sensitive", "echo 'This contains Error: but wrong case'", 'ERROR:'),
        ("No space after keyword", "echo 'Linkinglibraries without space'", 'Linking '),
        ("Different word", "echo 'This contains running but not at start'", 'Running '),
    ]
    
    for test_name, command, filter_key in negative_tests:
        output_file = f"negative_test_{test_name.replace(' ', '_').replace(':', '')}.txt"
        content = run_minas_test(test_name, command, output_file)
        
        # Check if any content was logged (should be minimal - just header/footer)
        lines = content.split('\n')
        logged_lines = [line for line in lines if line.strip() and 
                       not line.startswith('Script started') and 
                       not line.startswith('Script done') and
                       not line.startswith('2025-')]
        
        if logged_lines:
            print(f"❌ FAIL: Found logged content when should be empty: {logged_lines}")
            # Mark this filter as having negative test issues
            if filter_key in results:
                results[filter_key]['negative'] = False
        else:
            print(f"✅ PASS: No content logged (as expected)")
            # Mark this filter as passing negative tests
            if filter_key in results:
                results[filter_key]['negative'] = True
    
    return results



def cleanup():
    """Clean up all test files."""
    print("\n\nCleaning up test files...")
    print("=" * 50)
    
    # Get all test files
    test_files = []
    for file in os.listdir("."):
        if file.startswith(("filter_test_", "negative_test_")) and file.endswith(".txt"):
            test_files.append(file)
    
    for file in test_files:
        if os.path.exists(file):
            os.remove(file)
            print(f"Removed {file}")
    
    print("Cleanup completed!")

def print_summary_table(results):
    """Print a summary table of test results."""
    
    print("\n\n" + "=" * 50)
    print("SUMMARY TABLE")
    print("=" * 50)
    
    print("filter      | stdout | stderr | negative")
    print("------------|--------|--------|----------")
    
    for filter_name, status in results.items():
        stdout_status = "ok" if status['stdout'] else "FAIL"
        stderr_status = "ok" if status['stderr'] else "FAIL"
        negative_status = "ok" if status['negative'] else "FAIL"
        
        # Format filter name to align properly
        filter_display = f"{filter_name:<11}"
        
        print(f"{filter_display} | {stdout_status:>6} | {stderr_status:>6} | {negative_status:>8}")
    
    print("=" * 50)

def main():
    """Run all tests."""
    
    # Check if minas exists
    if not os.path.exists("./minas"):
        print("Error: minas program not found!")
        print("Please build minas first with: make minas")
        sys.exit(1)
    
    print("Comprehensive minas Testing")
    print("=" * 50)
    
    # Run all test suites and collect results
    results = test_individual_filters()
    results = test_negative_cases(results)
    
    # Cleanup
    cleanup()
    
    # Print summary table
    print_summary_table(results)
    
    print("\n" + "=" * 50)
    print("All tests completed!")

if __name__ == "__main__":
    main() 