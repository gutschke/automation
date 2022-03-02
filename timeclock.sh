#!/bin/bash -e

# Very basic script that tries to execute passed time clock events for the
# current day. This is useful when loading a new configuration using the
# Lutron software, as doing so doesn't necessarily sychronize all the state
# transitions. This script isn't smart enough to deal with more complex
# conditional schedules. It just runs normal events that have passed since
# midnight of the current day.

# The script unconditionally reads the ".lutron.xml" file. This file only
# exists if "automation" has been run at least once since the most recent
# changes to the Lutron schema.

# Find the integration ID for the time clocks
id="$(xmllint --xpath //Timeclock .lutron.xml |
      sed 's/IntegrationID="\([0-9]*\)".*/=\1/;t1;d;:1;s/.*=//;q')"
[ -n "${id}" ] || { echo "Can't find time clocks" >&2; exit 1; }

# Parse all registered events
xmllint --format --xpath '//TimeClockEvent' .lutron.xml |
  sed 's/<TimeClockEvent/\
&/g'|
  sed 's/>.*//;/Mode="[^"]*Normal/t;d' |
  while read -r line; do
    # Extract attributes for each event
    while read kv; do
      eval "x${kv}"
    done <<<$(egrep '[a-zA-Z]+="[^"]*"' <<<${line// /
})
    # Only execute events that match the current weekday
    if ! [[ "${xDays}" =~ "$(date +%A)" ]]; then
      echo "Skipping event for ${xDays}" >&2
      continue
    fi
    # Handle both fixed events, and events relative to sunrise/sunset
    case "${xType}" in
      Fixed)
        if [ "${xTime}" '<' "$(date +%02H:%02M)" ]; then
          [[ "${xTime/:/}" =~ ^0*([0-9].*) ]] && time="${BASH_REMATCH[1]}"
          printf '%04u %4u\n' "${time}" "${xEventNumber}"
        fi
      ;;
      VariableOffset)
        [[ "${xOffset}" =~ ^"+"?("-"?[0-9][0-9]):([0-9][0-9])$ ]] ||
          echo "Unexpect offset ${xOffset}" >&2
        case "${xVariableEvent}" in
          Sunrise|Sunset)
            time="$(hmadd "${xVariableEvent,,}" \
                          "${BASH_REMATCH[1]}${BASH_REMATCH[2]}")"
            [[ "${time}" =~ ^0*([0-9].*) ]] && time="${BASH_REMATCH[1]}"
            if [ "${time}" '<' "$(date +%02H:%02M)" ]; then
              printf '%04u %4u\n' "${time}" "${xEventNumber}"
            fi
            ;;
          *) echo "Unexpected event time ${xVariableEvent}" >&2
            ;;
        esac
      ;;
      *) echo "Unknown Type ${xType}" >&2;;
    esac
  done | sort -un | while read time event; do
    # Execute all events that have passed since midnight in chronological order
    lutron "#TIMECLOCK,${id},5,${event}"
  done
