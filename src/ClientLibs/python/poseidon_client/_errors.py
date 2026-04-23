"""Poseidon error codes and exception class."""


class PoseidonError(Exception):
    """Error returned by the Poseidon C library."""

    def __init__(self, message: str, code: int):
        super().__init__(message)
        self.code = code


_ERROR_MESSAGES = {
    0: "OK",
    1: "Unknown method",
    2: "Invalid parameters",
    3: "Channel not found",
    4: "Alias is ambiguous",
    5: "Not authorized",
    6: "Channel already exists",
    7: "Too many channels",
    8: "Transport error",
}


def error_from_code(code: int) -> PoseidonError:
    """Create a PoseidonError from a numeric error code."""
    message = _ERROR_MESSAGES.get(code, f"Unknown error ({code})")
    return PoseidonError(message, code)