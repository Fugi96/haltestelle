# pytest-embedded driver. It flashes this test_app, opens the serial port, then
# walks the Unity menu running every registered TEST_CASE and asserting each
# passes. The C side is the source of truth; this file is just the runner.
#
# Change the @pytest.mark.<target> marker to whatever chip you flash (esp32,
# esp32s3, esp32c3, ...). It must match the app you built.
import pytest
from pytest_embedded import Dut


@pytest.mark.esp32
def test_wlink_sta(dut: Dut) -> None:
    # Reads the case list off the menu and runs them all on this single board.
    dut.run_all_single_board_cases()
