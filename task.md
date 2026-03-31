currently in the @resources/web/homepage/index.html, the login button login into OrcaCloud if use_orca_cloud is set to true, and login into bambu cloud if false.
in this task, we will make following changes:
1. the current login button will always login into OrcaCloud
2. add collapsible second in the proper place to allow user login into Bambu cloud
3. user should be able be able to login into both cloud and keep the login status at the same time

Note: 
1. use_orca_cloud can be removed as the main cloud servicer will always be Orca Cloud
2. the BambuCloudSection in the frontend will be hidden if installed_networking is false

------------------
New requirements:
For future scalability, we want to add a new app configuration setting called `cloud_providers` that allows multiple cloud providers to be specified. By default, it includes "orca," and we can add "bambu." When "bambu" is included in the configuration, the Bambu login section will be displayed. Currently, the C++ backend supports only Orca and Bambu. The cloud agent should be refactored to support configuring multiple cloud providers.

Updated product decision: the visibility of BambuCloudSection will be solely decided by whether "bambu" is in `cloud_providers`.